// FusionUnit 实现（见 context/conversationFusion/FusionContext.h）

#include "context/conversationFusion/FusionContext.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "context/summarizor/SummaryManager.h"
#include "core/message/History.h"
#include "log/Log.h"

namespace mio {

namespace {

constexpr int kSummaryCount = 20;  // 摘要前 20 条
constexpr int kRawKeep = 5;        // 保留近 5 条原文（可调）

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

void FusionUnit::removeInDegree(const ConversationKey& key) {
    auto& v = fusion_.inDegree;
    v.erase(std::remove(v.begin(), v.end(), key), v.end());
}

CompressResult FusionUnit::compress(SummaryManager& summaryManager,
                                    const std::vector<Msg>& context,
                                    int summaryCount, int rawKeep) {
    const auto full = history::repairToolHistory(context);
    const auto tailStart = history::safeTailStart(full, rawKeep);
    CompressResult result;
    if (tailStart == 0 || summaryCount <= 0) {
        result.msgs = full;
        return result;
    }
    const std::size_t summarizeN =
        std::min<std::size_t>(static_cast<std::size_t>(summaryCount), tailStart);
    std::vector<Msg> toSummarize(full.begin(), full.begin() + summarizeN);
    SummaryOutcome outcome;
    try {
        outcome = summaryManager.summarizeWithVerdict(toSummarize);
        if (outcome.text.empty()) throw std::runtime_error("摘要为空");
    } catch (const std::exception& error) {
        log::warn("FusionUnit", std::string("上下文摘要失败，保留原历史: ") + error.what());
        result.msgs = full;
        return result;
    }

    Msg summaryMsg;
    summaryMsg.role = Role::User;
    summaryMsg.text = outcome.text;
    result.msgs.push_back(std::move(summaryMsg));
    result.msgs.insert(result.msgs.end(), full.begin() + tailStart, full.end());
    // privateVerdict = true 表示私密，isPublic 取反。
    result.isPublic = !outcome.privateVerdict;
    result.topic = outcome.topic;
    return result;
}

std::vector<Msg> FusionUnit::beRedirected(SummaryManager& summaryManager) {
    CompressResult r =
        compress(summaryManager, fusion_.context, kSummaryCount, kRawKeep);
    setIsPublic(r.isPublic);  // 摘要判定回写（重定向内容是否私密）
    if (!r.topic.empty())
        setTopic(std::move(r.topic));  // 话题随摘要同步更新
    return std::move(r.msgs);
}

std::vector<Msg> FusionUnit::beRedirected() {
    const auto full = history::repairToolHistory(fusion_.context);
    const auto start = history::safeTailStart(full, kRawKeep);
    return std::vector<Msg>(full.begin() + start, full.end());
}

void FusionUnit::append(Msg msg) {
    fusion_.context.push_back(std::move(msg));
}

void FusionUnit::stripEphemeralParts() {
    for (auto& msg : fusion_.context) {
        if (msg.parts.empty()) continue;
        msg.parts.erase(
            std::remove_if(msg.parts.begin(), msg.parts.end(),
                           [](const Part& p) {
                               return p.ephemeral || p.kind == Part::Kind::Image;
                           }),
            msg.parts.end());
    }
}

void FusionUnit::reColdStart(SummaryManager& summaryManager) {
    CompressResult r =
        compress(summaryManager, fusion_.context, kSummaryCount, kRawKeep);
    fusion_.context = std::move(r.msgs);
    setIsPublic(r.isPublic);
    if (!r.topic.empty())
        setTopic(std::move(r.topic));  // 话题随摘要同步更新
}

} // namespace mio
