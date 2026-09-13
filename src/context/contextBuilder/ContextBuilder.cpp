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

void ContextBuilder::applyColdStart(FusionUnit& unit, const BuildInput& in,
                                    BuildResult& out) {
    // 无论配置是否为 0 都标记完成：0 = 明确"不恢复历史原文"，不是"以后再说"
    unit.markColdStartDone();
    if (coldSource_ == nullptr) return;
    if (cfg_.coldStartRawTokens <= 0 || cfg_.coldStartMaxMessages <= 0) return;
    const auto& indeg = unit.inDegree();
    if (indeg.empty()) return;

    // 当前请求及输出预留预算优先：systemPrompt 与当前回合先占，
    // 剩余额度才用于恢复历史（历史不得挤掉当前请求）
    const std::int64_t reserved =
        estimateTokens(in.systemPrompt) + estimateTokens(unit.context());
    const std::int64_t remaining =
        static_cast<std::int64_t>(cfg_.coldStartRawTokens) - reserved;
    if (remaining <= 0) {
        log::info("ContextBuilder",
                  "冷启动预算已被 systemPrompt/当前回合占满，不恢复历史原文: unit=" +
                      indeg.front().toString());
        return;
    }

    ColdStartOptions opts;
    opts.rawTokenBudget = static_cast<int>(std::min<std::int64_t>(remaining, 1'000'000));
    opts.maxMessages = cfg_.coldStartMaxMessages;
    // 当前批次已在 unit 上下文里：按 messageId 排除，避免重复加入
    for (const auto& m : unit.context()) {
        if (m.messageId > 0) opts.excludeMessageIds.push_back(m.messageId);
    }

    const ColdStartResult cs =
        coldSource_->coldStart(indeg.front().toString(), opts);
    if (cs.msgs.empty()) return;

    // 当前批次可能已落盘但还没有 messageId（Runtime 先落档案再 build）：除按 ID
    // 排除外，再按 (role, text, createdAt) 去重，确保当前回合不重复加入
    std::vector<Msg> recovered;
    recovered.reserve(cs.msgs.size());
    for (const auto& r : cs.msgs) {
        bool duplicate = false;
        for (const auto& m : unit.context()) {
            if (m.role == r.role && m.text == r.text && m.createdAt == r.createdAt) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) recovered.push_back(r);
    }
    if (recovered.empty()) return;

    // 历史前置、当前回合保持末尾（时间正序；不修改档案、不碰协议数据）
    std::vector<Msg> merged;
    merged.reserve(recovered.size() + unit.context().size());
    merged.insert(merged.end(), recovered.begin(), recovered.end());
    merged.insert(merged.end(), unit.context().begin(), unit.context().end());
    unit.replaceContext(std::move(merged));

    out.coldStarted = true;
    out.coldStartRecovered = static_cast<int>(recovered.size());
    out.coldStartDropped = cs.droppedByBudget;
    out.coldStartTruncated = cs.anyTruncated;
    log::info("ContextBuilder",
              "冷启动恢复历史原文: unit=" + indeg.front().toString() +
                  " 条数=" + std::to_string(recovered.size()) +
                  " 跳过当前批次=" + std::to_string(cs.msgs.size() - recovered.size()) +
                  " 丢弃=" + std::to_string(cs.droppedByBudget) +
                  " 截断=" + (cs.anyTruncated ? "true" : "false") +
                  " 工具回合剔除=" + std::to_string(cs.droppedToolRounds));
}

BuildResult ContextBuilder::build(const BuildInput& in) {
    if (!in.unit) throw std::runtime_error("ContextBuilder: unit 为空");
    FusionUnit& unit = *in.unit;

    BuildResult r;
    // F3：unit 尚未冷启动 → 先按双重预算恢复历史原文（无 LLM、不写长期记忆）。
    // FusionRouter 建 unit 时已完成冷启动，这里只对直接构造的 unit 兜底。
    if (!unit.coldStartDone()) applyColdStart(unit, in, r);

    // 完整候选 = unit 上下文（当前回合已由调用方 append 进来，勿重复加入）
    std::vector<Msg> msgs = unit.context();
    std::int64_t estimated =
        estimateTokens(msgs) + estimateTokens(in.systemPrompt);

    const bool overBudget =
        estimated >
        static_cast<std::int64_t>(cfg_.budgetTokens * cfg_.watermark);
    if (overBudget && !msgs.empty() && summary_) {
        // 上下文不够 → 按 context_compaction 重新冷启动（分块全覆盖）后重建。
        // 该路径只做压缩：不写经历、不推进静默游标、不提升可见性。
        unit.reColdStart(*summary_);
        msgs = unit.context();
        estimated = estimateTokens(msgs) + estimateTokens(in.systemPrompt);
        r.reColdStarted = true;
        log::info("ContextBuilder", "上下文超水位，触发重新冷启动压缩: 压缩后估算=" +
                                        std::to_string(estimated));
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
