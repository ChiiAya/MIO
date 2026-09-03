#include "core/conversation/Conversation.h"

#include <utility>

namespace mio {

ConversationKey ConversationKey::privateChat(std::string id) {
    return {ConversationScope::Private, std::move(id)};
}

ConversationKey ConversationKey::groupChat(std::string id) {
    return {ConversationScope::Group, std::move(id)};
}

std::string ConversationKey::toString() const {
    const char* scopeName = scope == ConversationScope::Private ? "private" : "group";
    return platform + std::string(scopeName) + ":" + id;
}

bool ConversationKey::operator==(const ConversationKey& other) const {
    return scope == other.scope && id == other.id && platform == other.platform;
}

} // namespace mio
