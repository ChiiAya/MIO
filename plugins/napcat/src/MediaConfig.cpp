#include "adapters/napcat/MediaConfig.h"

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

MediaConfig MediaConfig::fromEnvironment() {
    MediaConfig cfg;
    if (auto v = env("MIO_MEDIA_ENABLED")) cfg.enabled = truthy(*v);
    if (auto v = env("MIO_MEDIA_CACHE_DIR")) cfg.cacheDir = *v;
    if (auto v = env("MIO_MEDIA_MAX_BYTES")) {
        try {
            cfg.maxFileBytes = static_cast<std::size_t>(std::stoull(*v));
        } catch (...) {
        }
    }
    cfg.asr = AsrConfig::fromEnvironment();
    return cfg;
}

void to_json(nlohmann::json& j, const MediaConfig& c) {
    j = nlohmann::json{
        {"enabled", c.enabled},
        {"images", c.images},
        {"audio", c.audio},
        {"video", c.video},
        {"files", c.files},
        {"persistImages", c.persistImages},
        {"maxFileBytes", c.maxFileBytes},
        {"downloadTimeoutMs", c.downloadTimeoutMs},
        {"cacheDir", c.cacheDir},
        {"asr", c.asr}
    };
}

void from_json(const nlohmann::json& j, MediaConfig& c) {
    readIfExists(j, "enabled", c.enabled);
    readIfExists(j, "images", c.images);
    readIfExists(j, "audio", c.audio);
    readIfExists(j, "video", c.video);
    readIfExists(j, "files", c.files);
    readIfExists(j, "persistImages", c.persistImages);
    readIfExists(j, "maxFileBytes", c.maxFileBytes);
    readIfExists(j, "downloadTimeoutMs", c.downloadTimeoutMs);
    readIfExists(j, "cacheDir", c.cacheDir);
    if (j.contains("asr") && j["asr"].is_object()) {
        from_json(j["asr"], c.asr);
    }
}

} // namespace mio
