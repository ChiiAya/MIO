#pragma once

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/message/Message.h"

namespace mio::napcat {

inline nlohmann::json buildSendParams(const Outbound& message) {
    auto segments = nlohmann::json::array();
    if (!message.replyTo.empty()) {
        segments.push_back({{"type", "reply"},
                            {"data", {{"id", message.replyTo}}}});
    }
    for (const auto& id : message.mentions) {
        if (id.empty()) continue;
        segments.push_back({{"type", "at"}, {"data", {{"qq", id}}}});
    }
    if (!message.text.empty()) {
        segments.push_back({{"type", "text"}, {"data", {{"text", message.text}}}});
    }

    bool group = false;
    std::string rawId = message.channelId;
    if (rawId.rfind("group:", 0) == 0) {
        group = true;
        rawId = rawId.substr(6);
    } else if (rawId.rfind("private:", 0) == 0) {
        group = false;
        rawId = rawId.substr(8);
    }

    nlohmann::json targetId = rawId;
    if (!rawId.empty() &&
        std::all_of(rawId.begin(), rawId.end(),
                    [](unsigned char c) { return std::isdigit(c); })) {
        try {
            targetId = std::stoull(rawId);
        } catch (const std::exception&) {
        }
    }

    nlohmann::json params{{"message", std::move(segments)}};
    params["message_type"] = group ? "group" : "private";
    params[group ? "group_id" : "user_id"] = std::move(targetId);
    return params;
}

} // namespace mio::napcat
