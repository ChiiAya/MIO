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
        {"topicWarmupTurns", c.topicWarmupTurns},
        {"coldStartRawTokens", c.coldStartRawTokens},
        {"coldStartMaxMessages", c.coldStartMaxMessages}
    };
}

void from_json(const nlohmann::json& j, ContextBuilderConfig& c) {
    readIfExists(j, "budgetTokens", c.budgetTokens);
    readIfExists(j, "watermark", c.watermark);
    readIfExists(j, "topicWarmupTurns", c.topicWarmupTurns);

    // 冷启动预算为新增字段：类型/范围错误必须整次重载失败（不得部分套用），
    // 缺失则保持默认值。
    if (j.contains("coldStartRawTokens") && !j["coldStartRawTokens"].is_null()) {
        if (!j["coldStartRawTokens"].is_number_integer())
            throw std::runtime_error("contextBuilder.coldStartRawTokens 必须是整数");
        c.coldStartRawTokens = j["coldStartRawTokens"].get<int>();
    }
    if (j.contains("coldStartMaxMessages") && !j["coldStartMaxMessages"].is_null()) {
        if (!j["coldStartMaxMessages"].is_number_integer())
            throw std::runtime_error("contextBuilder.coldStartMaxMessages 必须是整数");
        c.coldStartMaxMessages = j["coldStartMaxMessages"].get<int>();
    }
    if (c.coldStartRawTokens < 0)
        throw std::runtime_error("contextBuilder.coldStartRawTokens 不得为负");
    if (c.coldStartMaxMessages < 0)
        throw std::runtime_error("contextBuilder.coldStartMaxMessages 不得为负");
    if (c.coldStartRawTokens > c.budgetTokens)
        throw std::runtime_error(
            "contextBuilder.coldStartRawTokens 不得超过 budgetTokens（总上下文预算）");
}

void to_json(nlohmann::json& j, const MemoryConfig& c) {
    j = nlohmann::json{
        {"dim", c.dim},
        {"maxCandidates", c.maxCandidates},
        {"topK", c.topK},
        {"minSimilarity", c.minSimilarity},
        {"decayTauSeconds", c.decayTauSeconds},
        {"backend", c.backend},
        {"writeVisibilityCap", toString(c.writeVisibilityCap)},
        {"maxSummaryBytes", c.maxSummaryBytes},
        {"promptMaxBytes", c.promptMaxBytes},
        {"promptEntryMaxBytes", c.promptEntryMaxBytes}
    };
}

void from_json(const nlohmann::json& j, MemoryConfig& c) {
    readIfExists(j, "dim", c.dim);
    readIfExists(j, "maxCandidates", c.maxCandidates);
    readIfExists(j, "topK", c.topK);
    readIfExists(j, "minSimilarity", c.minSimilarity);
    readIfExists(j, "decayTauSeconds", c.decayTauSeconds);

    // 经历记忆后端名称：类型错误整次重载失败；未知名称由工厂降级为 unavailable
    // （降级是运行期行为，不是配置非法 —— 不得因此让整个配置重载失败）。
    if (j.contains("backend") && !j["backend"].is_null()) {
        if (!j["backend"].is_string())
            throw std::runtime_error("memory.backend 必须是字符串");
        c.backend = j["backend"].get<std::string>();
    }

    // 记忆写入可见性上限：只允许 conversation / person（当前阶段模型与摘要
    // 都不得放宽为 public），未知值整次重载失败。
    if (j.contains("writeVisibilityCap") && !j["writeVisibilityCap"].is_null()) {
        if (!j["writeVisibilityCap"].is_string())
            throw std::runtime_error("memory.writeVisibilityCap 必须是字符串");
        Visibility v = Visibility::Conversation;
        if (!parseVisibility(j["writeVisibilityCap"].get<std::string>(), v))
            throw std::runtime_error("memory.writeVisibilityCap 取值非法（conversation/person/public）");
        if (v == Visibility::Public)
            throw std::runtime_error(
                "memory.writeVisibilityCap 不得为 public（模型与摘要不得放宽可见性）");
        c.writeVisibilityCap = v;
    }
    if (j.contains("maxSummaryBytes") && !j["maxSummaryBytes"].is_null()) {
        if (!j["maxSummaryBytes"].is_number_integer())
            throw std::runtime_error("memory.maxSummaryBytes 必须是整数");
        const auto v = j["maxSummaryBytes"].get<long long>();
        if (v <= 0) throw std::runtime_error("memory.maxSummaryBytes 必须为正");
        c.maxSummaryBytes = static_cast<std::size_t>(v);
    }
    if (j.contains("promptMaxBytes") && !j["promptMaxBytes"].is_null()) {
        if (!j["promptMaxBytes"].is_number_integer())
            throw std::runtime_error("memory.promptMaxBytes 必须是整数");
        const auto v = j["promptMaxBytes"].get<long long>();
        if (v <= 0) throw std::runtime_error("memory.promptMaxBytes 必须为正");
        c.promptMaxBytes = static_cast<std::size_t>(v);
    }
    if (j.contains("promptEntryMaxBytes") && !j["promptEntryMaxBytes"].is_null()) {
        if (!j["promptEntryMaxBytes"].is_number_integer())
            throw std::runtime_error("memory.promptEntryMaxBytes 必须是整数");
        const auto v = j["promptEntryMaxBytes"].get<long long>();
        if (v <= 0) throw std::runtime_error("memory.promptEntryMaxBytes 必须为正");
        c.promptEntryMaxBytes = static_cast<std::size_t>(v);
    }
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
        {"inputBuffer", c.inputBuffer},
        {"conversation", toJson(c.conversation)}
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
    // conversation 节：局部热更新时缺失字段保持原值；非法则整次失败
    if (j.contains("conversation") && !j["conversation"].is_null()) {
        applyConversationLifecycleJson(j["conversation"], c.conversation);
    }
}

bool AppConfig::validate(std::string* error) const {
    const auto v = validateConversationLifecycle(conversation);
    if (!v.ok) {
        if (error) *error = v.error;
        return false;
    }
    if (contextBuilder.coldStartRawTokens < 0 || contextBuilder.coldStartMaxMessages < 0) {
        if (error) *error = "contextBuilder 冷启动预算不得为负";
        return false;
    }
    if (contextBuilder.coldStartRawTokens > contextBuilder.budgetTokens) {
        if (error) *error = "contextBuilder.coldStartRawTokens 不得超过 budgetTokens";
        return false;
    }
    return true;
}

bool AppConfig::applyEnvironment(std::string* error) {
    if (auto v = env("MIO_BOT_NAME")) botName = *v;
    if (auto v = env("MIO_DATA_DIR")) dataDir = *v;
    if (auto v = env("MIO_PLATFORM")) platform = *v;
    if (auto v = env("MIO_CHARACTER")) character = *v;
    if (auto v = env("MIO_SYSTEM_PROMPT_PREFIX")) systemPromptPrefix = *v;
    if (auto v = env("MIO_SYSTEM_PROMPT_NOTICE")) systemPromptNotice = *v;
    if (auto v = env("MIO_ADMIN_PORT")) {
        try { adminPort = std::stoi(*v); } catch (...) {}
    }
    if (const char* b = std::getenv("MIO_LLM_BACKEND")) {
        if (*b != '\0') llmBackend = b;
    }

    // 各子配置节：fromEnvironment 从默认值构造，这里按已存在的变量覆盖
    openai = OpenAiConfig::fromEnvironment();
    embedding = EmbeddingConfig::fromEnvironment();
    napcat = NapCatConfig::fromEnvironment();

    if (!applyConversationLifecycleEnv(conversation, error)) return false;
    // 经历记忆后端（只覆盖实际存在的变量；未知名称由工厂降级）
    if (auto v = env("MIO_MEMORY_BACKEND")) memory.backend = *v;
    if (error) error->clear();
    return true;
}

AppConfig AppConfig::fromEnvironment() {
    AppConfig cfg;
    cfg.llmBackend = llmBackendFromEnvironment();
    // 默认值 → 已提供的环境变量 → （由 ConfigManager 负责）JSON 字段覆盖
    cfg.applyEnvironment(nullptr);
    return cfg;
}

} // namespace mio

