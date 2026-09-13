// LocalMemoryProvider 实现（见 providers/memory/LocalMemoryProvider.h）
//
// 设计要点：
//   1) 映射是"纯函数 + 只收窄"：EpisodeMemory → SummaryRecord 逐字段复制，
//      visibility = narrower(请求, config 写上限)，不推测消息 ID、不代填 source；
//   2) 三道可见性闸门：MemoryManager 的 SQL 预过滤（第一道）、MemoryManager
//      返回前复核（第二道）、本适配器返回前复核 canSee + canAccessConversation
//      （第三道，最严：当前阶段跨会话一律不返回）；
//   3) 长度闸门：单条 ≤ cfg.promptEntryMaxBytes、总量 ≤ cfg.promptMaxBytes，
//      UTF-8 安全截断并写入可见标记；
//   4) 降级路径：任何异常 → 写入 Failed + STORAGE_UNAVAILABLE（localId 归零，
//      绝不返回 Stored/Queued）、召回空列表；绝不抛出到主对话链路。

#include "providers/memory/LocalMemoryProvider.h"

#include "log/Log.h"

#include <algorithm>
#include <ctime>
#include <exception>
#include <utility>

namespace mio {

namespace {

constexpr const char* kTag = "LocalMemoryProvider";

// 截断标记：既让模型/日志看得见"这条不完整"，也计入长度预算（断言据此成立）。
constexpr const char* kTruncationMark = "…[已截断]";

std::int64_t nowSeconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// 消息范围校验：0 < from <= to。非法即拒绝 —— 适配器不推测范围、不伪造 ID。
bool validRange(const EpisodeMemory& episode) {
    return episode.fromMessageId > 0 &&
           episode.toMessageId >= episode.fromMessageId;
}

// UTF-8 安全截断 + 可见标记；保证返回值的字节数 <= maxBytes（含标记）。
std::string truncateWithMark(const std::string& text, std::size_t maxBytes,
                             bool* truncated) {
    if (text.size() <= maxBytes) {
        if (truncated != nullptr) *truncated = false;
        return text;
    }
    if (truncated != nullptr) *truncated = true;
    const std::string mark = kTruncationMark;
    if (maxBytes <= mark.size()) return limits::truncateUtf8(mark, maxBytes);
    return limits::truncateUtf8(text, maxBytes - mark.size()) + mark;
}

} // namespace

const std::vector<std::string>& LocalMemoryProvider::allowedIncludes() {
    // 依赖白名单：只允许 MemoryManager（唯一数据入口）、MemoryProvider（契约）、
    // Limits（预算）与日志。刻意不含任何文件系统/档案/图谱头。
    static const std::vector<std::string> kAllowed = {
        "core/contracts/Limits.h",
        "log/Log.h",
        "memory/manager/MemoryManager.h",
        "providers/memory/MemoryProvider.h",
    };
    return kAllowed;
}

LocalMemoryProvider::LocalMemoryProvider(MemoryManager& memory, MemoryConfig config)
    : memory_(memory), cfg_(config) {}

std::string LocalMemoryProvider::name() const { return "sqlite-local"; }

bool LocalMemoryProvider::available() const { return true; }

SummaryRecord LocalMemoryProvider::toRecord(const EpisodeMemory& episode,
                                            std::int64_t now) const {
    SummaryRecord rec;
    rec.conversationKey = episode.conversationKey;
    rec.participants = episode.participants;
    rec.kind = episode.kind;
    rec.summary = episode.text;
    // 可见性只可收窄：请求值与 config 写上限取更窄一侧；MemoryManager 内部还会
    // 再按它自己的 writeVisibilityCap 收窄一次（两道都收窄，绝不放大）。
    rec.visibility = narrower(episode.visibility, cfg_.writeVisibilityCap);
    rec.createdAt = episode.createdAt > 0 ? episode.createdAt : now;
    rec.eventTime = episode.eventTime > 0 ? episode.eventTime : rec.createdAt;
    rec.fromMessageId = episode.fromMessageId;
    rec.toMessageId = episode.toMessageId;
    // source 是溯源强制字段：调用方必须给出真实来源；为空时由
    // validateSummaryRecord 拒绝（适配器不代填来源，避免伪造溯源）。
    rec.source = episode.source;
    rec.idempotencyKey = episode.idempotencyKey;
    // 向量化在事务提交后由 MemoryManager 决定（ok → ok；失败 → failed + 待办）。
    rec.embeddingStatus = EmbeddingStatus::Pending;
    return rec;
}

StoreEpisodeResult LocalMemoryProvider::storeEpisode(const EpisodeMemory& episode) {
    StoreEpisodeResult out;
    try {
        // ---- 1) 幂等：localId > 0 表示服务端已知记录 ------------------------
        // 不盲信调用方给的 ID：记录必须存在且属同一会话，否则等于接受伪造 ID
        // （甚至可能是跨会话写入）。宁可 Rejected 也不假装成功。
        if (episode.localId > 0) {
            const auto existing = memory_.getSummary(episode.localId);
            if (!existing.has_value()) {
                out.status = StoreStatus::Rejected;
                out.code = ErrorCode::InvalidArgument;
                out.message = "localId 指向不存在的本地记录，拒绝按幂等处理: " +
                              std::to_string(episode.localId);
                return out;
            }
            if (!episode.conversationKey.empty() &&
                existing->conversationKey != episode.conversationKey) {
                out.status = StoreStatus::Rejected;
                out.code = ErrorCode::InvalidArgument;
                out.message = "localId 归属其它会话，拒绝跨会话幂等写入";
                return out;
            }
            out.status = StoreStatus::Duplicate;
            out.localId = episode.localId;
            out.code = ErrorCode::Ok;
            out.message = "已知本地记录：按幂等命中返回原 ID，不重复写入";
            return out;
        }

        // ---- 2) 校验：非法输入不落盘、不伪造 ID -----------------------------
        if (episode.conversationKey.empty()) {
            out.status = StoreStatus::Rejected;
            out.code = ErrorCode::InvalidArgument;
            out.message = "conversationKey 不得为空";
            return out;
        }
        if (episode.text.empty()) {
            out.status = StoreStatus::Rejected;
            out.code = ErrorCode::InvalidArgument;
            out.message = "经历正文不得为空";
            return out;
        }
        if (!validRange(episode)) {
            out.status = StoreStatus::Rejected;
            out.code = ErrorCode::InvalidArgument;
            out.message =
                "消息范围非法：需要 0 < fromMessageId <= toMessageId"
                "（from=" + std::to_string(episode.fromMessageId) +
                ", to=" + std::to_string(episode.toMessageId) +
                "）；适配器不推测范围、不伪造 ID";
            return out;
        }
        if (episode.text.size() > limits::kWriteMaxBytes) {
            out.status = StoreStatus::Rejected;
            out.code = ErrorCode::LimitExceeded;
            out.message = "经历正文超长：" + std::to_string(episode.text.size()) +
                          " > " + std::to_string(limits::kWriteMaxBytes) +
                          " 字节（写入超限返回 LIMIT_EXCEEDED，不静默截断）";
            return out;
        }

        // ---- 3) 映射并写入（唯一数据入口：MemoryManager）--------------------
        const SummaryRecord record = toRecord(episode, nowSeconds());
        const SummaryWriteResult w = memory_.writeSummary(record);
        if (!w.ok) {
            // 失败绝不返回 Stored/Queued，也绝不带 localId
            out.localId = 0;
            out.code = w.code;
            out.status = (w.code == ErrorCode::InvalidArgument ||
                          w.code == ErrorCode::LimitExceeded)
                             ? StoreStatus::Rejected
                             : StoreStatus::Failed;
            out.message = w.message.empty() ? "本地记忆写入失败" : w.message;
            log::warn(kTag, "storeEpisode 未写入: " + out.message);
            return out;
        }

        out.localId = w.memoryId;
        out.code = ErrorCode::Ok;
        out.status = w.duplicate ? StoreStatus::Duplicate : StoreStatus::Stored;
        out.message = w.message.empty() ? "已写入本地记忆库" : w.message;
        if (!w.duplicate) {
            // 文本已确认落盘（Stored 成立）；向量化 pending/failed 只是可重试状态，
            // 必须如实告知调用方，不能包装成"全部完成"。
            switch (w.embeddingStatus) {
            case EmbeddingStatus::Ok:
                break;
            case EmbeddingStatus::Pending:
                out.message += "；向量化待处理（待办已持久化）";
                break;
            case EmbeddingStatus::Failed:
                out.message += "；向量化失败（文本已保留，待办可重试）";
                break;
            }
        } else if (out.localId == 0) {
            // 游标覆盖命中但没有独立记录（例如空范围审计推进过游标）：
            // 如实报告"没有产生新记忆"，而不是编一个 ID。
            out.message = "该消息范围已被提交游标覆盖，未产生新记忆（无独立记录 ID）";
        }
        return out;
    } catch (const std::exception& e) {
        out.status = StoreStatus::Failed;
        out.localId = 0;
        out.code = ErrorCode::StorageUnavailable;
        out.message = std::string("本地记忆后端不可用，未写入: ") + e.what();
        log::warn(kTag, out.message);
        return out;
    } catch (...) {
        out.status = StoreStatus::Failed;
        out.localId = 0;
        out.code = ErrorCode::StorageUnavailable;
        out.message = "本地记忆后端不可用（未知异常），未写入";
        log::warn(kTag, out.message);
        return out;
    }
}

std::vector<MemoryHit> LocalMemoryProvider::recall(const RecallQuery& query) {
    std::vector<MemoryHit> out;
    try {
        if (query.text.empty()) return out;
        const std::size_t topK = query.topK > 0 ? query.topK : cfg_.topK;
        if (topK == 0) return out;  // topK=0 显式关闭召回
        // 门槛取"服务端 config"与"调用方要求"的更严者，防止绕过服务端下限。
        const double threshold = std::max(cfg_.minSimilarity, query.minSimilarity);

        // 转发给 MemoryManager：SQL 层可见性预过滤 + 默认排除 context_compaction
        // + 返回前二次复核（第一、二道闸门）。
        const std::vector<RecalledMemory> recalled =
            memory_.recall(query.text, query.access, topK,
                           query.allowContextCompaction);

        std::size_t totalBytes = 0;
        for (const auto& r : recalled) {
            // ---- 第三道闸门：返回前复核（不可见与不存在一律不返回）----------
            if (!query.access.canSee(r.visibility)) continue;
            if (!query.access.canAccessConversation(r.convKey)) continue;
            if (!query.allowContextCompaction &&
                r.kind == SummaryKind::ContextCompaction)
                continue;  // compaction 只有显式允许才召回
            if (r.similarity < threshold) continue;

            MemoryHit hit;
            hit.localId = r.id;
            hit.text = r.summary;
            hit.conversationKey = r.convKey;
            hit.visibility = r.visibility;
            hit.kind = r.kind;
            hit.score = r.score;
            hit.createdAt = r.createdAt;
            hit.source = r.source;

            bool truncated = false;
            // 单条上限
            hit.text = truncateWithMark(hit.text, cfg_.promptEntryMaxBytes, &truncated);
            // 总量上限：预算用尽就不再加条（宁可不注入，也不无界增长）
            if (totalBytes >= cfg_.promptMaxBytes) {
                log::warn(kTag, "召回总量预算用尽，剩余条目未返回（budget=" +
                                    std::to_string(cfg_.promptMaxBytes) + " 字节）");
                break;
            }
            const std::size_t room = cfg_.promptMaxBytes - totalBytes;
            if (hit.text.size() > room) {
                hit.text = truncateWithMark(hit.text, room, &truncated);
            }
            if (hit.text.empty()) break;
            totalBytes += hit.text.size();
            if (truncated) {
                log::warn(kTag, "召回条目超过长度预算已截断（localId=" +
                                    std::to_string(hit.localId) + "）");
            }
            out.push_back(std::move(hit));
        }
        return out;
    } catch (const std::exception& e) {
        // 后端异常内部消化：降级为"无长期召回"，不阻塞主对话链路
        log::warn(kTag, std::string("召回失败，降级为空结果: ") + e.what());
        return {};
    } catch (...) {
        log::warn(kTag, "召回失败（未知异常），降级为空结果");
        return {};
    }
}

} // namespace mio
