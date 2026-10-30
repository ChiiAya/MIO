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

void to_json(nlohmann::json& j, const AchieveConfig& c) {
    j = nlohmann::json{
        {"enableColdSummary", c.enableColdSummary},
        {"summaryCount", c.summaryCount},
        {"rawKeep", c.rawKeep},
        {"inlineMediaMaxBytes", c.inlineMediaMaxBytes}
    };
}

void from_json(const nlohmann::json& j, AchieveConfig& c) {
    readIfExists(j, "enableColdSummary", c.enableColdSummary);
    readIfExists(j, "summaryCount", c.summaryCount);
    readIfExists(j, "rawKeep", c.rawKeep);
    readIfExists(j, "inlineMediaMaxBytes", c.inlineMediaMaxBytes);
}

void to_json(nlohmann::json& j, const ContextBuilderConfig& c) {
    j = nlohmann::json{
        {"budgetTokens", c.budgetTokens},
        {"watermark", c.watermark}
    };
}

void from_json(const nlohmann::json& j, ContextBuilderConfig& c) {
    readIfExists(j, "budgetTokens", c.budgetTokens);
    readIfExists(j, "watermark", c.watermark);
}

void to_json(nlohmann::json& j, const MessageSplitterConfig& c) {
    j = nlohmann::json{
        {"enabled", c.enabled},
        {"delimiter", c.delimiter},
        {"baseDelayMs", c.baseDelayMs},
        {"delayPerCharMs", c.delayPerCharMs},
        {"maxDelayMs", c.maxDelayMs}
    };
}

void from_json(const nlohmann::json& j, MessageSplitterConfig& c) {
    readIfExists(j, "enabled", c.enabled);
    readIfExists(j, "delimiter", c.delimiter);
    readIfExists(j, "baseDelayMs", c.baseDelayMs);
    readIfExists(j, "delayPerCharMs", c.delayPerCharMs);
    readIfExists(j, "maxDelayMs", c.maxDelayMs);
}

void to_json(nlohmann::json& j, const PluginSpec& c) {
    j = nlohmann::json{
        {"id", c.id},
        {"enabled", c.enabled},
        {"path", c.path},
        {"config", c.config}
    };
}

void from_json(const nlohmann::json& j, PluginSpec& c) {
    readIfExists(j, "id", c.id);
    readIfExists(j, "enabled", c.enabled);
    readIfExists(j, "path", c.path);
    if (j.contains("config") && j["config"].is_object()) {
        c.config = j["config"];
    }
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
        {"mindDebounceMs", c.mindDebounceMs},
        {"openai", c.openai},
        {"embedding", c.embedding},
        {"achieve", c.achieve},
        {"contextBuilder", c.contextBuilder},
        {"splitter", c.splitter},
        {"mcp", c.mcp},
        {"maintenance", c.maintenance},
        {"memoryBridge", c.memoryBridge},
        {"plugins", c.plugins}
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
    readIfExists(j, "mindDebounceMs", c.mindDebounceMs);

    if (j.contains("openai") && j["openai"].is_object()) {
        from_json(j["openai"], c.openai);
    }
    if (j.contains("embedding") && j["embedding"].is_object()) {
        from_json(j["embedding"], c.embedding);
    }
    if (j.contains("achieve") && j["achieve"].is_object()) {
        from_json(j["achieve"], c.achieve);
    }
    if (j.contains("contextBuilder") && j["contextBuilder"].is_object()) {
        from_json(j["contextBuilder"], c.contextBuilder);
    }
    if (j.contains("splitter") && j["splitter"].is_object()) {
        from_json(j["splitter"], c.splitter);
    }
    if (j.contains("mcp") && j["mcp"].is_object()) {
        from_json(j["mcp"], c.mcp);
    }
    if (j.contains("maintenance") && j["maintenance"].is_object()) {
        from_json(j["maintenance"], c.maintenance);
    }
    if (j.contains("memoryBridge") && j["memoryBridge"].is_object()) {
        from_json(j["memoryBridge"], c.memoryBridge);
    }
    if (j.contains("plugins") && j["plugins"].is_array()) {
        c.plugins.clear();
        for (const auto& item : j["plugins"]) {
            if (item.is_object()) {
                PluginSpec spec;
                from_json(item, spec);
                if (!spec.id.empty()) {
                    c.plugins.push_back(std::move(spec));
                }
            }
        }
    } else if (j.contains("napcat") && j["napcat"].is_object()) {
        // 兼容旧版顶层 napcat 字段：自动更新到 plugins["napcat"].config
        bool found = false;
        for (auto& spec : c.plugins) {
            if (spec.id == "napcat") {
                spec.config = j["napcat"];
                spec.enabled = (c.platform == "napcat" || c.platform == "all" || c.platform == "both");
                found = true;
            } else if (spec.id == "console") {
                spec.enabled = (c.platform == "console" || c.platform == "all" || c.platform == "both");
            }
        }
        if (!found) {
            c.plugins.push_back(PluginSpec{
                "napcat",
                c.platform == "napcat" || c.platform == "all" || c.platform == "both",
                "",
                j["napcat"]
            });
        }
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
    if (auto v = env("MIO_MIND_DEBOUNCE_MS")) {
        try { cfg.mindDebounceMs = std::stoi(*v); } catch (...) {}
    }
    cfg.llmBackend = llmBackendFromEnvironment();

    cfg.openai = OpenAiConfig::fromEnvironment();
    cfg.embedding = EmbeddingConfig::fromEnvironment();
    cfg.achieve = AchieveConfig{};
    cfg.contextBuilder = ContextBuilderConfig{};
    cfg.splitter = MessageSplitterConfig{};

    const bool enableConsole = (cfg.platform == "console" || cfg.platform == "all" || cfg.platform == "both");
    const bool enableNapcat  = (cfg.platform == "napcat"  || cfg.platform == "all" || cfg.platform == "both");
    cfg.plugins = {
        PluginSpec{"console", enableConsole, "", nlohmann::json::object()},
        PluginSpec{"napcat", enableNapcat, "", nlohmann::json::object()},
        PluginSpec{"lua-host", true, "", nlohmann::json{{"dir", "plugins/lua"}}}
    };

    return cfg;
}

} // namespace mio
