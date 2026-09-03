#include "config/openai/OpenaiConfig.h"

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <algorithm>
namespace mio {

namespace {

    
std::optional<std::string> env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}
    
std::optional<mio::ReasoningEffort> ReasoningEffortFromString(const std::string& name) {
    
    if (name.empty()) {
        return std::nullopt;
    }

    static const std::unordered_map<std::string, mio::ReasoningEffort> map = {
        {"low",    mio::ReasoningEffort::Low},
        {"medium", mio::ReasoningEffort::Medium},
        {"high",   mio::ReasoningEffort::High},
        {"xhigh",  mio::ReasoningEffort::XHigh},
        {"max",    mio::ReasoningEffort::Max},
        // 可选：Max 一般不作为有效输入，如果需要可加上 {"max", mio::ReasoningEffort::Max}
    };

    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = map.find(key);
    if (it != map.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<mio::Thinking> ThinkingFromString(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }

    static const std::unordered_map<std::string, mio::Thinking> map = {
        {"enabled",  mio::Thinking::Enabled},
        {"disabled", mio::Thinking::Disabled},
    };

    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = map.find(key);
    if (it != map.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<int> envInt(const char* name) {
    if (auto v = env(name)) {
        try {
            return std::stoi(*v);
        } catch (...) {
        }
    }
    return std::nullopt;
}

} // namespace

OpenAiConfig OpenAiConfig::fromEnvironment() {
    OpenAiConfig cfg;
    if (auto v = env("MIO_BASE_URL")) cfg.baseUrl = *v;
    if (auto v = env("MIO_API_KEY")) cfg.apiKey = *v;
    if (auto v = env("MIO_MODEL")) cfg.model = *v;
    if (auto v = envInt("MIO_MAX_TOKENS")) cfg.maxTokens = *v;
    if (auto v = env("MIO_MODEL_REASONING_EFFORT")) cfg.reasoningEffort = ReasoningEffortFromString(*v);
    if (auto v = env("MIO_THINKING")) cfg.thinking = ThinkingFromString(*v);
    return cfg;
}

std::string llmBackendFromEnvironment() {
    return env("MIO_LLM_BACKEND").value_or("openai");
}

} // namespace mio