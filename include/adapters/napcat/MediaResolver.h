#pragma once

#include <memory>
#include <string>
#include <vector>

#include "adapters/napcat/MediaConfig.h"
#include "adapters/napcat/OneBotTypes.h"
#include "core/event/Event.h"

namespace mio {

class AsrProvider;

class MediaResolver {
public:
    MediaResolver(MediaConfig cfg, std::shared_ptr<AsrProvider> asr = nullptr);

    // 解析附件，将转写/缓存提示追加到 ev.content，并将图片填充到 ev.parts
    void resolve(const std::vector<MediaAttachment>& attachments, Event& ev) const;

private:
    bool kindEnabled(MediaKind kind) const;
    std::string download(const MediaAttachment& attachment) const;

    MediaConfig cfg_;
    std::shared_ptr<AsrProvider> asr_;
};

} // namespace mio
