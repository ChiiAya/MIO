#pragma once
// ============================================================================
// ContextBuilder（上下文构建器）：
//   1) 接受信息（unit 上下文 + 当前回合 + systemPrompt），转换为 ChatRequest；
//   2) 上下文不够时触发压缩：对 unit 重新冷启动（[摘要前20 + 近5原文]）后重建请求。
// ============================================================================

#include <ctime>
#include <memory>
#include <string>

#include "context/conversationFusion/FusionContext.h"
#include "context/summarizor/SummaryManager.h"
#include "providers/llm/Llm.h"
#include "core/message/Message.h"

namespace mio {

struct ContextBuilderConfig {
    int budgetTokens = 16'000;   // 预算（超水位触发重新冷启动）
    double watermark = 0.85;
    int topicWarmupTurns = 5;    // unit 创建后前 N 轮提醒模型补 update_topic
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
};

class ContextBuilder {
public:
    ContextBuilder(ContextBuilderConfig cfg, std::shared_ptr<SummaryManager> summary);

    // 构建请求；上下文超水位且 unit 非空时 → 重新冷启动压缩
    BuildResult build(const BuildInput& in);

private:
    ContextBuilderConfig cfg_;
    std::shared_ptr<SummaryManager> summary_;
};

} // namespace mio
