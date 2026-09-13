#include "config/AppConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mio {

namespace {

std::optional<std::string> env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

template <typename T>
void readIfExists(const nlohmann::json& j, const char* key, T& target) {
    if (j.contains(key) && !j[key].is_null()) {
        try {
            target = j[key].get<T>();
        } catch (...) {
        }
    }
}

template <typename T>
void readIfExists(const nlohmann::json& j, const char* key, std::optional<T>& target) {
    if (j.contains(key)) {
        if (j[key].is_null()) {
            target = std::nullopt;
        } else {
            try {
                target = j[key].get<T>();
            } catch (...) {
            }
        }
    }
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::optional<ReasoningEffort> parseReasoningEffort(const std::string& str) {
    const std::string lower = toLower(str);
    if (lower == "low") return ReasoningEffort::Low;
    if (lower == "medium") return ReasoningEffort::Medium;
    if (lower == "high") return ReasoningEffort::High;
    if (lower == "xhigh") return ReasoningEffort::XHigh;
    if (lower == "max") return ReasoningEffort::Max;
    return std::nullopt;
}

std::string reasoningEffortToString(ReasoningEffort effort) {
    switch (effort) {
    case ReasoningEffort::Low: return "low";
    case ReasoningEffort::Medium: return "medium";
    case ReasoningEffort::High: return "high";
    case ReasoningEffort::XHigh: return "xhigh";
    case ReasoningEffort::Max: return "max";
    }
    return "medium";
}

std::optional<Thinking> parseThinking(const std::string& str) {
    const std::string lower = toLower(str);
    if (lower == "enabled") return Thinking::Enabled;
    if (lower == "disabled") return Thinking::Disabled;
    return std::nullopt;
}

std::string thinkingToString(Thinking t) {
    switch (t) {
    case Thinking::Enabled: return "enabled";
    case Thinking::Disabled: return "disabled";
    }
    return "disabled";
}

} // namespace

void to_json(nlohmann::json& j, const OpenAiConfig& c) {
    j = nlohmann::json{
        {"baseUrl", c.baseUrl},
        {"apiKey", c.apiKey},
        {"model", c.model},
        {"maxRetries", c.maxRetries},
        {"retryInitialDelayMs", c.retryInitialDelayMs},
        {"retryBackoffFactor", c.retryBackoffFactor},
        {"httpTimeoutMs", c.httpTimeoutMs}
    };
    if (c.temperature.has_value()) j["temperature"] = *c.temperature;
    if (c.topP.has_value()) j["topP"] = *c.topP;
    if (c.maxTokens.has_value()) j["maxTokens"] = *c.maxTokens;
    if (c.reasoningEffort.has_value()) {
        j["reasoningEffort"] = reasoningEffortToString(*c.reasoningEffort);
    }
    if (c.thinking.has_value()) {
        j["thinking"] = thinkingToString(*c.thinking);
    }
}

void from_json(const nlohmann::json& j, OpenAiConfig& c) {
    readIfExists(j, "baseUrl", c.baseUrl);
    readIfExists(j, "apiKey", c.apiKey);
    readIfExists(j, "model", c.model);
    readIfExists(j, "temperature", c.temperature);
    readIfExists(j, "topP", c.topP);
    readIfExists(j, "maxTokens", c.maxTokens);
    readIfExists(j, "maxRetries", c.maxRetries);
    readIfExists(j, "retryInitialDelayMs", c.retryInitialDelayMs);
    readIfExists(j, "retryBackoffFactor", c.retryBackoffFactor);
    readIfExists(j, "httpTimeoutMs", c.httpTimeoutMs);

    if (j.contains("reasoningEffort")) {
        if (j["reasoningEffort"].is_null()) {
            c.reasoningEffort = std::nullopt;
        } else if (j["reasoningEffort"].is_string()) {
            c.reasoningEffort = parseReasoningEffort(j["reasoningEffort"].get<std::string>());
        }
    }

    if (j.contains("thinking")) {
        if (j["thinking"].is_null()) {
            c.thinking = std::nullopt;
        } else if (j["thinking"].is_string()) {
            c.thinking = parseThinking(j["thinking"].get<std::string>());
        }
    }
}

void to_json(nlohmann::json& j, const EmbeddingConfig& c) {
    j = nlohmann::json{
        {"baseUrl", c.baseUrl},
        {"apiKey", c.apiKey},
        {"model", c.model},
        {"maxRetries", c.maxRetries},
        {"retryInitialDelayMs", c.retryInitialDelayMs},
        {"retryBackoffFactor", c.retryBackoffFactor},
        {"httpTimeoutMs", c.httpTimeoutMs}
    };
}

void from_json(const nlohmann::json& j, EmbeddingConfig& c) {
    readIfExists(j, "baseUrl", c.baseUrl);
    readIfExists(j, "apiKey", c.apiKey);
    readIfExists(j, "model", c.model);
    readIfExists(j, "maxRetries", c.maxRetries);
    readIfExists(j, "retryInitialDelayMs", c.retryInitialDelayMs);
    readIfExists(j, "retryBackoffFactor", c.retryBackoffFactor);
    readIfExists(j, "httpTimeoutMs", c.httpTimeoutMs);
}

void to_json(nlohmann::json& j, const FusionConfig& c) {
    j = nlohmann::json{
        {"intimacyThreshold", c.intimacyThreshold},
        {"trustThreshold", c.trustThreshold},
        {"topicSemanticMatch", c.topicSemanticMatch},
        {"topicSimilarityThreshold", c.topicSimilarityThreshold}
    };
}

void from_json(const nlohmann::json& j, FusionConfig& c) {
    readIfExists(j, "intimacyThreshold", c.intimacyThreshold);
    readIfExists(j, "trustThreshold", c.trustThreshold);
    readIfExists(j, "topicSemanticMatch", c.topicSemanticMatch);
    readIfExists(j, "topicSimilarityThreshold", c.topicSimilarityThreshold);
}

void to_json(nlohmann::json& j, const ContextBuilderConfig& c) {
    j = nlohmann::json{
        {"budgetTokens", c.budgetTokens},
        {"watermark", c.watermark},
        {"topicWarmupTurns", c.topicWarmupTurns}
    };
}

void from_json(const nlohmann::json& j, ContextBuilderConfig& c) {
    readIfExists(j, "budgetTokens", c.budgetTokens);
    readIfExists(j, "watermark", c.watermark);
    readIfExists(j, "topicWarmupTurns", c.topicWarmupTurns);
}

void to_json(nlohmann::json& j, const MemoryConfig& c) {
    j = nlohmann::json{
        {"dim", c.dim},
        {"maxCandidates", c.maxCandidates},
        {"topK", c.topK},
        {"minSimilarity", c.minSimilarity},
        {"decayTauSeconds", c.decayTauSeconds}
    };
}

void from_json(const nlohmann::json& j, MemoryConfig& c) {
    readIfExists(j, "dim", c.dim);
    readIfExists(j, "maxCandidates", c.maxCandidates);
    readIfExists(j, "topK", c.topK);
    readIfExists(j, "minSimilarity", c.minSimilarity);
    readIfExists(j, "decayTauSeconds", c.decayTauSeconds);
}

void to_json(nlohmann::json& j, const NapCatConfig& c) {
    j = nlohmann::json{
        {"listenHost", c.listenHost},
        {"listenPort", c.listenPort},
        {"token", c.token}
    };
}

void from_json(const nlohmann::json& j, NapCatConfig& c) {
    readIfExists(j, "listenHost", c.listenHost);
    readIfExists(j, "listenPort", c.listenPort);
    readIfExists(j, "token", c.token);
}

void to_json(nlohmann::json& j, const InputBufferConfig& c) {
    j = nlohmann::json{
        {"maxDelayMs", c.maxDelayMs},
        {"debounceMs", c.debounceMs},
        {"maxTextLength", c.maxTextLength},
        {"maxBatchSize", c.maxBatchSize}
    };
}

void from_json(const nlohmann::json& j, InputBufferConfig& c) {
    readIfExists(j, "maxDelayMs", c.maxDelayMs);
    readIfExists(j, "debounceMs", c.debounceMs);
    readIfExists(j, "maxTextLength", c.maxTextLength);
    readIfExists(j, "maxBatchSize", c.maxBatchSize);
}

void to_json(nlohmann::json& j, const AppConfig& c) {
    j = nlohmann::json{
        {"botName", c.botName},
        {"dataDir", c.dataDir},
        {"platform", c.platform},
        {"llmBackend", c.llmBackend},
        {"character", c.character},
        {"systemPromptPrefix", c.systemPromptPrefix},
        {"systemPromptNotice", c.systemPromptNotice},
        {"adminPort", c.adminPort},
        {"openai", c.openai},
        {"embedding", c.embedding},
        {"fusion", c.fusion},
        {"contextBuilder", c.contextBuilder},
        {"memory", c.memory},
        {"napcat", c.napcat},
        {"inputBuffer", c.inputBuffer}
    };
}

void from_json(const nlohmann::json& j, AppConfig& c) {
    readIfExists(j, "botName", c.botName);
    readIfExists(j, "dataDir", c.dataDir);
    readIfExists(j, "platform", c.platform);
    readIfExists(j, "llmBackend", c.llmBackend);
    readIfExists(j, "character", c.character);
    readIfExists(j, "systemPromptPrefix", c.systemPromptPrefix);
    readIfExists(j, "systemPromptNotice", c.systemPromptNotice);
    readIfExists(j, "adminPort", c.adminPort);

    if (j.contains("openai") && j["openai"].is_object()) {
        from_json(j["openai"], c.openai);
    }
    if (j.contains("embedding") && j["embedding"].is_object()) {
        from_json(j["embedding"], c.embedding);
    }
    if (j.contains("fusion") && j["fusion"].is_object()) {
        from_json(j["fusion"], c.fusion);
    }
    if (j.contains("contextBuilder") && j["contextBuilder"].is_object()) {
        from_json(j["contextBuilder"], c.contextBuilder);
    }
    if (j.contains("memory") && j["memory"].is_object()) {
        from_json(j["memory"], c.memory);
    }
    if (j.contains("napcat") && j["napcat"].is_object()) {
        from_json(j["napcat"], c.napcat);
    }
    if (j.contains("inputBuffer") && j["inputBuffer"].is_object()) {
        from_json(j["inputBuffer"], c.inputBuffer);
    }
}

AppConfig AppConfig::fromEnvironment() {
    AppConfig cfg;
    if (auto v = env("MIO_BOT_NAME")) cfg.botName = *v;
    if (auto v = env("MIO_DATA_DIR")) cfg.dataDir = *v;
    if (auto v = env("MIO_PLATFORM")) cfg.platform = *v;
    if (auto v = env("MIO_CHARACTER")) cfg.character = *v;
    if (auto v = env("MIO_SYSTEM_PROMPT_PREFIX")) cfg.systemPromptPrefix = *v;
    if (auto v = env("MIO_SYSTEM_PROMPT_NOTICE")) cfg.systemPromptNotice = *v;
    if (auto v = env("MIO_ADMIN_PORT")) {
        try { cfg.adminPort = std::stoi(*v); } catch (...) {}
    }
    cfg.llmBackend = llmBackendFromEnvironment();

    cfg.openai = OpenAiConfig::fromEnvironment();
    cfg.embedding = EmbeddingConfig::fromEnvironment();
    cfg.fusion = FusionConfig{};
    cfg.contextBuilder = ContextBuilderConfig{};
    cfg.memory = MemoryConfig{};
    cfg.napcat = NapCatConfig::fromEnvironment();
    cfg.inputBuffer = InputBufferConfig{};

    return cfg;
}

} // namespace mio

