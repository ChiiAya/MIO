#pragma once
// ============================================================================
// ContextBuilder（单意识流上下文构建器）：
//   1) 接受单意识流上下文 (std::vector<Msg>&) + systemPrompt，转换为 ChatRequest；
//   2) 上下文超水位时触发滚动压缩（摘要旧消息 + 保留尾部 rawKeep 条原文）。
// ============================================================================

#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "context/summarizor/SummaryManager.h"
#include "core/message/Message.h"
#include "providers/llm/Llm.h"

namespace mio {

struct ContextBuilderConfig {
    int budgetTokens = 16'000;
    double watermark = 0.85;
    int topicWarmupTurns = 5;
    int rawKeep = 8;
};

struct BuildInput {
    std::string systemPrompt;
    std::vector<Msg>* context = nullptr;
    std::time_t now = 0;
};

struct BuildResult {
    ChatRequest request;
    bool reColdStarted = false;
};

class ContextBuilder {
public:
    ContextBuilder(ContextBuilderConfig cfg, std::shared_ptr<SummaryManager> summary);

    BuildResult build(const BuildInput& in);

private:
    ContextBuilderConfig cfg_;
    std::shared_ptr<SummaryManager> summary_;
};

} // namespace mio
