#pragma once
// ============================================================================
// ContextBuilder（上下文构建器）：
//   1) 接受信息（unit 上下文 + 当前回合 + systemPrompt），转换为 ChatRequest；
//   2) 冷启动（F3）：unit 尚未冷启动时，在双重预算内从档案恢复最近可见原文
//      —— 不调用摘要模型、不写长期记忆；coldStartRawTokens / coldStartMaxMessages
//      任一为 0 表示【不恢复历史原文】；
//   3) 上下文不够时触发压缩：按 kind = context_compaction 重新冷启动后重建请求。
//
// 预算优先级：systemPrompt 与当前回合先占用预算，剩余额度才用于恢复历史
// （见 applyColdStart），因此不会为了历史原文挤掉当前请求。
// ============================================================================

#include <ctime>
#include <memory>
#include <string>

#include "context/achieve/ColdStart.h"
#include "context/conversationFusion/FusionContext.h"
#include "context/summarizor/SummaryManager.h"
#include "providers/llm/Llm.h"
#include "core/message/Message.h"

namespace mio {

struct ContextBuilderConfig {
    int budgetTokens = 16'000;   // 预算（超水位触发重新冷启动）
    double watermark = 0.85;
    int topicWarmupTurns = 5;    // unit 创建后前 N 轮提醒模型补 update_topic
    // 冷启动原文恢复预算（实施默认值，允许 0 = 不恢复历史原文）：
    // 在双重预算内按时间正序恢复最近可见消息，且不得同步调用摘要模型。
    int coldStartRawTokens = 2000;
    int coldStartMaxMessages = 20;
};

struct BuildInput {
    std::string systemPrompt;  // Facts 渲染（重建时机由调用方负责）
    FusionUnit* unit = nullptr;   // 调用方须已持有 unit->mtx，
                                  // 且当前回合已 append 进 unit（勿重复传）
    std::time_t now = 0;
};

struct BuildResult {
    ChatRequest request;
    bool reColdStarted = false;  // 触发了重新冷启动（调用方消费日记并重建 Facts）
    // 冷启动（F3）可观察信息：恢复条数、被预算丢弃条数、是否发生 UTF-8 截断
    bool coldStarted = false;
    int coldStartRecovered = 0;
    int coldStartDropped = 0;
    bool coldStartTruncated = false;
};

class ContextBuilder {
public:
    ContextBuilder(ContextBuilderConfig cfg, std::shared_ptr<SummaryManager> summary);

    // 冷启动数据源（可空依赖，Achieve 实现 IColdStartSource）：
    // 注入后，未冷启动的 unit 在首次 build 时按双重预算恢复历史原文。
    void setColdStartSource(IColdStartSource* source) { coldSource_ = source; }

    // 构建请求；上下文超水位且 unit 非空时 → 重新冷启动压缩
    BuildResult build(const BuildInput& in);

private:
    // 冷启动兜底：只用剩余预算恢复历史原文（无 LLM、无长期记忆写入），
    // 结果前置到 unit 上下文（当前回合保持在末尾，不重复加入）。
    void applyColdStart(FusionUnit& unit, const BuildInput& in, BuildResult& out);

    ContextBuilderConfig cfg_;
    std::shared_ptr<SummaryManager> summary_;
    IColdStartSource* coldSource_ = nullptr;
};

} // namespace mio
