#pragma once

#include <string>

namespace mio {

enum class ConversationScope {
    Private,
    Group,
};

struct ConversationKey {
    ConversationScope scope;
    std::string id;
    std::string platform;

    static ConversationKey privateChat(std::string id);
    static ConversationKey groupChat(std::string id);

    std::string toString() const;
    bool operator==(const ConversationKey& other) const;
};

} // namespace mio
