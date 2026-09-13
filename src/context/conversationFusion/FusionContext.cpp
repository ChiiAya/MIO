// FusionUnit 实现（见 context/conversationFusion/FusionContext.h）
//
// 压缩（F4）硬约束：
//   * kind = context_compaction，source = context_compaction；
//   * 不写经历、不推进静默游标、不提升可见性（只可收窄，用 narrower()）；
//   * 摘要材料剔除 reasoning（副本上清空，不碰 unit 里的协议数据）；
//   * 目标范围【分块全覆盖】：逐块摘要并全部拼进结果；确实丢弃时把
//     未覆盖范围写进正文标记 + CompressResult.uncoveredRanges，不静默。

#include "context/conversationFusion/FusionContext.h"

#include <algorithm>
#include <ctime>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "context/achieve/ColdStart.h"  // isFrameworkReminder
#include "context/costEstimator/Tokens.h"
#include "context/summarizor/SummaryManager.h"
#include "core/contracts/Visibility.h"
#include "log/Log.h"
#include "providers/llm/Llm.h"

namespace mio {

namespace {

constexpr int kRawKeepDegrade = 5;  // 无 LLM 降级：只保留最近 5 条原文

std::string unitLabel(const FusionContext& f) {
    return f.inDegree.empty() ? "?" : f.inDegree.front().toString();
}

std::string hexEncode(const std::string& s) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 0x0F];
    }
    return out;
}

Visibility unitVisibility(const FusionContext& f) {
    return f.isPublic ? Visibility::Public : Visibility::Conversation;
}

// 目标范围按 token 预算切块：块与块首尾相接，[0, count) 无缝覆盖
std::vector<std::pair<std::size_t, std::size_t>> splitByTokens(
    const std::vector<Msg>& context, std::size_t count, int chunkTokens) {
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
    if (count == 0) return chunks;
    const std::int64_t budget = std::max<std::int64_t>(1, chunkTokens);
    std::size_t begin = 0;
    std::int64_t acc = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t cost = estimateTokens(context[i]);
        // 单条超预算的消息独占一块（保证覆盖，不会死循环）
        if (i > begin && acc + cost > budget) {
            chunks.emplace_back(begin, i);
            begin = i;
            acc = 0;
        }
        acc += cost;
    }
    chunks.emplace_back(begin, count);
    return chunks;
}

// 摘要材料：副本 + 剔除 reasoning（不修改 unit 里的消息）。
// 工具调用/结果保留（摘要需要知道发生过什么），但正文里的 reasoning 一律清空。
std::vector<Msg> buildMaterial(const std::vector<Msg>& context, std::size_t begin,
                               std::size_t end) {
    std::vector<Msg> material;
    material.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        const Msg& src = context[i];
        if (src.role != Role::User && src.role != Role::Assistant &&
            src.role != Role::Tool)
            continue;  // 系统消息/旧摘要不进材料
        if (src.isSummary) continue;
        if (isFrameworkReminder(src.text)) continue;
        Msg m = src;                 // 副本：不破坏工具循环所需的协议数据
        m.reasoningContent.clear();  // 硬约束：reasoning 永不进入摘要材料
        m.parts.clear();             // 多模态大载荷不进材料
        m.truncated = false;
        material.push_back(std::move(m));
    }
    return material;
}

std::string rangeLabel(const std::vector<Msg>& context, std::size_t begin,
                       std::size_t end) {
    // 优先用真实 messageId 定位；未分配（0）时回退为块内序号，便于人工排障
    std::int64_t from = 0;
    std::int64_t to = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const std::int64_t id = context[i].messageId;
        if (id <= 0) continue;
        if (from == 0) from = id;
        to = id;
    }
    if (from > 0) return "消息 " + std::to_string(from) + "-" + std::to_string(to);
    return "第 " + std::to_string(begin + 1) + "-" + std::to_string(end) + " 条";
}

std::pair<std::int64_t, std::int64_t> rangeIds(const std::vector<Msg>& context,
                                               std::size_t begin,
                                               std::size_t end) {
    std::int64_t from = 0;
    std::int64_t to = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const std::int64_t id = context[i].messageId;
        if (id <= 0) continue;
        if (from == 0) from = id;
        to = id;
    }
    return {from, to};
}

// 工具协议自洽化：窗口内 assistant 的每个 toolCallId 必须有对应结果，
// 每个工具结果也必须能对上窗口内的调用；否则剔除。避免把历史/窗口边缘的
// 工具调用当成未完成调用重放（孤立 tool 消息会让下一次请求直接失败）。
int sanitizeToolProtocol(std::vector<Msg>& window) {
    std::unordered_set<std::string> results;
    for (const auto& m : window) {
        if (m.role == Role::Tool && !m.toolCallId.empty())
            results.insert(m.toolCallId);
    }
    std::unordered_set<std::string> liveCalls;
    std::vector<Msg> kept;
    kept.reserve(window.size());
    int dropped = 0;
    for (auto& m : window) {
        if (m.role == Role::Assistant && !m.toolCalls.empty()) {
            bool closed = !results.empty();
            for (const auto& tc : m.toolCalls) {
                if (tc.id.empty() || results.count(tc.id) == 0) {
                    closed = false;
                    break;
                }
            }
            if (!closed) {  // 未闭合的调用：整条剔除
                ++dropped;
                continue;
            }
            for (const auto& tc : m.toolCalls) liveCalls.insert(tc.id);
            kept.push_back(std::move(m));
            continue;
        }
        if (m.role == Role::Tool) {
            if (m.toolCallId.empty() || liveCalls.count(m.toolCallId) == 0) {
                ++dropped;  // 孤立工具结果
                continue;
            }
        }
        kept.push_back(std::move(m));
    }
    window = std::move(kept);
    return dropped;
}

// 取上下文尾部 keep 条，并清理工具协议边缘
std::vector<Msg> tailWindow(const std::vector<Msg>& context, std::size_t keep) {
    if (keep == 0 || context.empty()) return {};
    const std::size_t n = std::min(keep, context.size());
    std::vector<Msg> window(context.end() - static_cast<long>(n), context.end());
    const int dropped = sanitizeToolProtocol(window);
    if (dropped > 0)
        log::warn("FusionUnit", "尾部窗口剔除 " + std::to_string(dropped) +
                                    " 条不成对的工具协议消息");
    return window;
}

} // namespace

FusionUnit::FusionUnit(const ConversationKey self, std::vector<Msg> context)
    : fusion_{{std::move(self)}, std::move(context), "", false} {}

void FusionUnit::setTopic(std::string topic) {
    const std::string oldTopic = fusion_.topic;
    fusion_.topic = std::move(topic);
    log::info("FusionUnit",
              "topic 更新: unit=" + unitLabel(fusion_) +
                  " old=" + oldTopic +
                  " new=" + fusion_.topic +
                  " new_hex=" + hexEncode(fusion_.topic) +
                  " isPublic=" + (fusion_.isPublic ? "true" : "false"));
}

void FusionUnit::setIsPublic(bool isPublic) {
    const bool old = fusion_.isPublic;
    fusion_.isPublic = isPublic;
    log::info("FusionUnit",
              "isPublic 更新: unit=" + unitLabel(fusion_) +
                  " old=" + (old ? "true" : "false") +
                  " new=" + (fusion_.isPublic ? "true" : "false") +
                  " topic=" + fusion_.topic);
}

void FusionUnit::mergeParticipants(const std::vector<std::string>& more) {
    for (const auto& p : more) {
        if (p.empty()) continue;
        if (std::find(fusion_.participants.begin(), fusion_.participants.end(),
                      p) == fusion_.participants.end())
            fusion_.participants.push_back(p);
    }
}

void FusionUnit::removeInDegree(const ConversationKey& key) {
    auto& v = fusion_.inDegree;
    v.erase(std::remove(v.begin(), v.end(), key), v.end());
}

CompressResult FusionUnit::compress(SummaryManager& summaryManager,
                                    const std::vector<Msg>& context,
                                    const CompressOptions& opts) {
    CompressResult result;
    const std::size_t n = context.size();
    const std::size_t rawKeep =
        static_cast<std::size_t>(std::max(0, opts.rawKeep));
    if (n <= rawKeep) {
        // 不足 rawKeep 条：只保留原文，不调用 LLM 摘要
        result.msgs = context;
        return result;
    }

    const std::size_t targetCount = n - rawKeep;
    std::vector<std::pair<std::size_t, std::size_t>> chunks =
        splitByTokens(context, targetCount, opts.chunkTokens);

    // 分块上限：超出时只压缩最近的块，更旧的块显式标记未覆盖
    const std::size_t maxChunks =
        static_cast<std::size_t>(std::max(1, opts.maxChunks));
    std::size_t firstChunk = 0;
    if (chunks.size() > maxChunks) {
        firstChunk = chunks.size() - maxChunks;
        result.fullCoverage = false;
        for (std::size_t i = 0; i < firstChunk; ++i) {
            const auto ids = rangeIds(context, chunks[i].first, chunks[i].second);
            result.uncoveredRanges.push_back(ids);
        }
    }

    const std::string convKey =
        fusion_.inDegree.empty() ? "" : fusion_.inDegree.front().toString();
    std::ostringstream merged;
    bool anyPrivate = false;
    const std::size_t total = chunks.size() - firstChunk;

    for (std::size_t i = firstChunk; i < chunks.size(); ++i) {
        const std::size_t b = chunks[i].first;
        const std::size_t e = chunks[i].second;
        const std::size_t seq = i - firstChunk + 1;
        const std::string label = rangeLabel(context, b, e);
        const auto ids = rangeIds(context, b, e);

        // 独立生成 context_compaction：不写经历、不推进静默游标
        SummaryRequest req;
        req.kind = SummaryKind::ContextCompaction;
        req.source = opts.source.empty() ? "context_compaction" : opts.source;
        req.conversationKey = convKey;
        req.participants = fusion_.participants;
        req.visibility = Visibility::Conversation;  // 最窄：压缩不提升可见性
        req.now = static_cast<std::int64_t>(std::time(nullptr));
        req.allowVisibilityNarrowing = true;
        req.material = buildMaterial(context, b, e);
        req.fromMessageId = ids.first;
        req.toMessageId = ids.second;

        const SummaryOutcome outcome = summaryManager.summarize(req);
        ++result.chunks;
        if (!outcome.text.empty()) {
            merged << "[压缩 " << seq << "/" << total << " · " << label << "]\n"
                   << outcome.text << "\n";
        } else {
            // 摘要失败：范围未被覆盖，必须显式写进正文 + 可观察字段
            ++result.failedChunks;
            result.fullCoverage = false;
            result.uncoveredRanges.push_back(ids);
            merged << "[压缩 " << seq << "/" << total << " · " << label
                   << "] （该范围摘要失败，未覆盖："
                   << (outcome.error.empty() ? "未知原因" : outcome.error)
                   << "）\n";
        }
        if (outcome.privateVerdict) anyPrivate = true;
        if (!outcome.topic.empty()) result.topic = outcome.topic;
    }

    if (firstChunk > 0) {
        merged << "[压缩] 更早的 " << firstChunk
               << " 个分块因超出压缩预算未覆盖（范围见 uncoveredRanges）\n";
    }

    Msg summaryMsg;
    summaryMsg.role = Role::User;
    summaryMsg.text = merged.str();
    result.msgs.push_back(std::move(summaryMsg));
    result.msgs.insert(result.msgs.end(), context.end() - static_cast<long>(rawKeep),
                       context.end());
    // 建议可见性：任一块判定私密 → 私密；调用方只允许用它收窄
    result.isPublic = !anyPrivate;
    return result;
}

std::vector<Msg> FusionUnit::beRedirected(SummaryManager& summaryManager,
                                          CompressOptions opts) {
    CompressResult r = compress(summaryManager, fusion_.context, opts);
    // 只可收窄：摘要建议公开不能把私密 unit 变公开
    setIsPublic(narrower(unitVisibility(fusion_),
                         r.isPublic ? Visibility::Public : Visibility::Conversation) ==
                Visibility::Public);
    if (!r.topic.empty()) setTopic(std::move(r.topic));  // 话题随摘要同步更新
    if (!r.fullCoverage) {
        log::warn("FusionUnit", "重定向压缩存在未覆盖范围: unit=" +
                                    unitLabel(fusion_) + " chunks=" +
                                    std::to_string(r.chunks) + " failed=" +
                                    std::to_string(r.failedChunks));
    }
    return std::move(r.msgs);
}

std::vector<Msg> FusionUnit::beRedirected() {
    // 无 LLM 的降级：仅返回近 5 条原文（工具协议边缘已清理）
    return tailWindow(fusion_.context, kRawKeepDegrade);
}

void FusionUnit::append(Msg msg) {
    fusion_.context.push_back(std::move(msg));
}

CompressResult FusionUnit::reColdStart(SummaryManager& summaryManager,
                                       CompressOptions opts) {
    CompressResult r = compress(summaryManager, fusion_.context, opts);
    fusion_.context = r.msgs;  // 保留 r.msgs 供调用方观察（不 move）
    // 只可收窄
    setIsPublic(narrower(unitVisibility(fusion_),
                         r.isPublic ? Visibility::Public : Visibility::Conversation) ==
                Visibility::Public);
    if (!r.topic.empty()) setTopic(std::move(r.topic));  // 话题随摘要同步更新
    if (!r.fullCoverage) {
        log::warn("FusionUnit", "重新冷启动压缩存在未覆盖范围: unit=" +
                                    unitLabel(fusion_) + " chunks=" +
                                    std::to_string(r.chunks) + " failed=" +
                                    std::to_string(r.failedChunks));
    }
    return r;
}

void FusionUnit::recordUsage(const Usage& usage) {
    std::lock_guard<std::mutex> lock(statsMtx_);
    stats_.totalPromptTokens += usage.inputOther;
    stats_.totalCachedTokens += usage.inputCached;
    stats_.totalOutputTokens += usage.output;
    stats_.lastPromptTokens = usage.inputOther;
    stats_.lastCachedTokens = usage.inputCached;
    stats_.lastOutputTokens = usage.output;
    stats_.lastCacheHit = (usage.inputCached > 0);
    stats_.requestCount += 1;
}

} // namespace mio
