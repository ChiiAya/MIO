#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mio {

enum class ReasoningEffort {
    Low,
    Medium,
    High,
    XHigh,
    Max
};

enum class Thinking {
    Enabled,
    Disabled
};

struct OpenAiConfig {
    // 适配器会自动补全路径：
    //   "http://127.0.0.1:8080"        -> .../v1/chat/completions
    //   "http://127.0.0.1:8080/v1"     -> .../v1/chat/completions
    //   Ollama 用户填 http://127.0.0.1:11434 即可
    std::string baseUrl = "http://127.0.0.1:8080";
    std::string apiKey;
    std::string model = "local-model";
    std::optional<double> temperature;
    std::optional<double> topP;
    std::optional<mio::ReasoningEffort> reasoningEffort;
    std::optional<mio::Thinking> thinking;
    std::optional<int> maxTokens = 512;
    int maxRetries = 2;                // 额外重试次数（首次之外）
    int retryInitialDelayMs = 1500;    // 指数退避初始延迟
    double retryBackoffFactor = 2.0;
    int httpTimeoutMs = 120'000;       // 单次请求超时

    // 从环境变量读取（不存在则用默认值）：
    //   MIO_BASE_URL / MIO_API_KEY / MIO_MODEL / MIO_MAX_TOKENS
    static OpenAiConfig fromEnvironment();
};

// 当前仅支持 "openai"（兼容协议）；留作未来接入其他协议族的选择开关
std::string llmBackendFromEnvironment();

} // namespace mio
