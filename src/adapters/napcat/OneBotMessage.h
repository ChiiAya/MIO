#pragma once

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/conversation/Conversation.h"
#include "core/message/Message.h"

namespace mio::napcat {

// A segment array keeps model-written CQ strings literal and preserves explicit
// reply/mention metadata independently of text escaping.
inline nlohmann::json buildSendParams(const ConversationKey& conversation,
                                      const OutgoingMessage& message) {
    auto segments = nlohmann::json::array();
    if (!message.replyToMessageId.empty()) {
        segments.push_back({{"type", "reply"},
                            {"data", {{"id", message.replyToMessageId}}}});
    }
    for (const auto& id : message.mentionUserIds) {
        if (id.empty()) continue;
        segments.push_back({{"type", "at"}, {"data", {{"qq", id}}}});
    }
    if (!message.text.empty()) {
        segments.push_back({{"type", "text"}, {"data", {{"text", message.text}}}});
    }
    nlohmann::json targetId = conversation.id;
    if (!conversation.id.empty() &&
        std::all_of(conversation.id.begin(), conversation.id.end(),
                    [](unsigned char c) { return std::isdigit(c); })) {
        try {
            targetId = std::stoull(conversation.id);
        } catch (const std::exception&) {
            // Keep oversized identifiers exact instead of rounding/truncating.
        }
    }
    nlohmann::json params{{"message", std::move(segments)}};
    const bool group = conversation.scope == ConversationScope::Group;
    params["message_type"] = group ? "group" : "private";
    params[group ? "group_id" : "user_id"] = std::move(targetId);
    return params;
}

} // namespace mio::napcat
