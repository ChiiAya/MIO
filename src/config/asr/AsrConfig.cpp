#include "config/asr/AsrConfig.h"

#include <cstdlib>
#include <optional>
#include <string>

namespace mio {

namespace {

std::optional<std::string> env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

bool truthy(const std::string& value) {
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
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

} // namespace

AsrConfig AsrConfig::fromEnvironment() {
    AsrConfig cfg;
    if (auto v = env("MIO_ASR_ENABLED")) cfg.enabled = truthy(*v);
    if (auto v = env("MIO_ASR_BASE_URL")) {
        cfg.baseUrl = *v;
        cfg.enabled = true;
    } else if (auto v = env("MIO_BASE_URL")) {
        cfg.baseUrl = *v;
    }
    if (auto v = env("MIO_ASR_API_KEY")) cfg.apiKey = *v;
    else if (auto v = env("MIO_API_KEY")) cfg.apiKey = *v;
    if (auto v = env("MIO_ASR_MODEL")) cfg.model = *v;
    if (auto v = env("MIO_ASR_LANGUAGE")) cfg.language = *v;
    return cfg;
}

void to_json(nlohmann::json& j, const AsrConfig& c) {
    j = nlohmann::json{
        {"enabled", c.enabled},
        {"baseUrl", c.baseUrl},
        {"apiKey", c.apiKey},
        {"model", c.model},
        {"language", c.language},
        {"httpTimeoutMs", c.httpTimeoutMs}
    };
}

void from_json(const nlohmann::json& j, AsrConfig& c) {
    readIfExists(j, "enabled", c.enabled);
    readIfExists(j, "baseUrl", c.baseUrl);
    readIfExists(j, "apiKey", c.apiKey);
    readIfExists(j, "model", c.model);
    readIfExists(j, "language", c.language);
    readIfExists(j, "httpTimeoutMs", c.httpTimeoutMs);
}

} // namespace mio
