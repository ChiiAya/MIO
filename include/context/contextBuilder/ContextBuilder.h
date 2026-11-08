#pragma once
// ============================================================================
// ContextBuilder（单意识流上下文构建器）：
//   1) 接受单意识流上下文 (std::vector<Msg>&) + systemPrompt，转换为 ChatRequest；
//   2) 上下文超水位时按 rawKeep 截断旧历史。
// ============================================================================

#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "core/message/Message.h"
#include "providers/llm/Llm.h"

namespace mio {

struct ContextBuilderConfig {
    int budgetTokens = 16'000;
    double watermark = 0.85;
    int rawKeep = 8;
};

struct BuildInput {
    std::string systemPrompt;
    std::vector<Msg>* context = nullptr;
};

struct BuildResult {
    ChatRequest request;
    bool reColdStarted = false;
};

class ContextBuilder {
public:
    explicit ContextBuilder(ContextBuilderConfig cfg);

    BuildResult build(const BuildInput& in);

private:
    ContextBuilderConfig cfg_;
};

} // namespace mio
