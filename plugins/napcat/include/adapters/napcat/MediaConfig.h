#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "config/asr/AsrConfig.h"

namespace mio {

enum class MediaKind {
    Image,
    Audio,
    Video,
    File,
};

struct MediaAttachment {
    MediaKind kind = MediaKind::Image;
    std::string url;
    std::string localPath;
    std::string fileId;
    std::string fileName;
    std::string mimeType;
    std::int64_t sizeBytes = 0;
    std::int64_t durationSec = 0;
};

struct MediaConfig {
    bool enabled = true;
    bool images = true;
    bool audio = true;
    bool video = false;
    bool files = false;

    std::size_t maxFileBytes = 8u * 1024u * 1024u;
    int downloadTimeoutMs = 15'000;
    std::string cacheDir = "data/media";

    AsrConfig asr;

    static MediaConfig fromEnvironment();
};

void to_json(nlohmann::json& j, const MediaConfig& c);
void from_json(const nlohmann::json& j, MediaConfig& c);

} // namespace mio
