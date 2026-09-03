#include "config/embedding/EmbeddingConfig.h"

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

} // namespace

EmbeddingConfig EmbeddingConfig::fromEnvironment() {
    EmbeddingConfig cfg;
    if (auto v = env("MIO_EMBED_BASE_URL")) cfg.baseUrl = *v;
    else if (auto v = env("MIO_BASE_URL")) cfg.baseUrl = *v;
    if (auto v = env("MIO_EMBED_API_KEY")) cfg.apiKey = *v;
    else if (auto v = env("MIO_API_KEY")) cfg.apiKey = *v;
    if (auto v = env("MIO_EMBED_MODEL")) cfg.model = *v;
    return cfg;
}

} // namespace mio
