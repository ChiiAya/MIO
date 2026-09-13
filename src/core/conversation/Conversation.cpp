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

std::optional<ConversationKey> ConversationKey::fromString(const std::string& s) {
    static const std::string kPrivate = "private:";
    static const std::string kGroup = "group:";
    const std::size_t p = s.find(kPrivate);
    const std::size_t g = s.find(kGroup);
    std::size_t at = std::string::npos;
    bool isPrivate = true;
    if (p == std::string::npos && g == std::string::npos) return std::nullopt;
    if (p == std::string::npos) {
        at = g;
        isPrivate = false;
    } else if (g == std::string::npos) {
        at = p;
    } else if (p <= g) {
        at = p;
    } else {
        at = g;
        isPrivate = false;
    }
    ConversationKey key;
    key.scope = isPrivate ? ConversationScope::Private : ConversationScope::Group;
    key.platform = s.substr(0, at);
    key.id = s.substr(at + (isPrivate ? kPrivate.size() : kGroup.size()));
    if (key.id.empty()) return std::nullopt;
    return key;
}

} // namespace mio
