#pragma once

#include "core/conversation/Conversation.h"
#include "providers/llm/tool/ToolRegistry.h"

namespace mio {

struct MessageToolContext {
    ConversationKey conversation;
    std::string platform;
    std::vector<std::string> replyableMessageIds;
};

using StructuredMessageSender =
    std::function<bool(const ConversationKey&, const OutgoingMessage&)>;

void registerMessageTools(ToolRegistry& registry,
                          std::function<MessageToolContext()> context,
                          StructuredMessageSender send);

} // namespace mio
