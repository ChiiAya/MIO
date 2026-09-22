// ContextBuilder 实现（见 context/contextBuilder/ContextBuilder.h）

#include "context/contextBuilder/ContextBuilder.h"

#include <algorithm>
#include <stdexcept>

#include "context/costEstimator/Tokens.h"
#include "log/Log.h"

namespace mio {

ContextBuilder::ContextBuilder(ContextBuilderConfig cfg,
                               std::shared_ptr<SummaryManager> summary)
    : cfg_(cfg), summary_(std::move(summary)) {}

BuildResult ContextBuilder::build(const BuildInput& in) {
    if (!in.unit) throw std::runtime_error("ContextBuilder: unit 为空");
    FusionUnit& unit = *in.unit;

    // 完整候选 = unit 上下文（当前回合已由调用方 append 进来，勿重复加入）
    std::vector<Msg> msgs = unit.context();

    // 保护：历史轮次（在最后一条 Assistant 消息之前的所有 User 消息）绝不重复上送图片数据，
    // 仅保留最新一轮未回复的 User 消息中携带的图片，彻底防止视觉模型在多轮对话中因历史图片干扰或冻结。
    std::size_t lastAssistantIdx = std::string::npos;
    for (std::size_t i = msgs.size(); i > 0; --i) {
        if (msgs[i - 1].role == Role::Assistant) {
            lastAssistantIdx = i - 1;
            break;
        }
    }
    if (lastAssistantIdx != std::string::npos) {
        for (std::size_t i = 0; i <= lastAssistantIdx; ++i) {
            if (msgs[i].parts.empty()) continue;
            msgs[i].parts.erase(
                std::remove_if(msgs[i].parts.begin(), msgs[i].parts.end(),
                               [](const Part& p) {
                                   return p.ephemeral || p.kind == Part::Kind::Image;
                               }),
                msgs[i].parts.end());
        }
    }
    std::int64_t estimated =
        estimateTokens(msgs) + estimateTokens(in.systemPrompt);

    BuildResult r;
    const bool overBudget =
        estimated >
        static_cast<std::int64_t>(cfg_.budgetTokens * cfg_.watermark);
    if (overBudget && !unit.context().empty() && summary_) {
        // 上下文不够 → 对 unit 重新冷启动（[摘要前20 + 近5原文]）后重建
        unit.reColdStart(*summary_);
        msgs = unit.context();
        estimated = estimateTokens(msgs) + estimateTokens(in.systemPrompt);
        r.reColdStarted = true;
        log::info("ContextBuilder", "上下文超水位，触发重新冷启动");
    }

    // topic 空白期预热：unit 创建后的前 N 轮（topic 一经填写即停止提示），
    // 请求末尾注入提醒，驱动模型尽快调用 update_topic
    if (unit.topic().empty() &&
        unit.turnsSinceCreation() <= cfg_.topicWarmupTurns) {
        Msg reminder{Role::User};
        reminder.text =
            "[框架]本会话还没有话题记录：请立即调用 update_topic "
            "工具，用一句话总结当前对话的话题。";
        msgs.push_back(std::move(reminder));
    }

    ChatRequest req;
    req.systemPrompt = in.systemPrompt;
    req.messages = std::move(msgs);
    r.request = std::move(req);
    return r;
}

} // namespace mio
