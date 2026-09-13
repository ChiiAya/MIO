#pragma once

#include <optional>
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

    // 反解 toString()：格式为 "<platform>private:<id>" / "<platform>group:<id>"
    // （platform 为空时退化为 "private:<id>" / "group:<id>"，与旧档案命名兼容）。
    // 平台名里若含 "private:"/"group:" 子串，以**第一次**出现处切分；无法识别返回 nullopt。
    static std::optional<ConversationKey> fromString(const std::string& s);
};

} // namespace mio
