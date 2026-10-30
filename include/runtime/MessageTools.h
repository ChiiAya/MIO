#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/message/Message.h"
#include "providers/llm/tool/ToolRegistry.h"

namespace mio {

struct MessageToolContext {
    std::string source;
    std::string channelId;
};

using OutboundSender = std::function<bool(const Outbound&)>;

void registerMessageTools(ToolRegistry& registry,
                          std::function<MessageToolContext()> context,
                          OutboundSender send);

} // namespace mio
