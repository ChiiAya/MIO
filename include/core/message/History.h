#pragma once

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/message/Message.h"

namespace mio::history {

inline Msg cancelledToolResult(const std::string& id) {
    Msg result{Role::Tool};
    result.toolCallId = id;
    result.text = "error: 工具调用已中断，未取得结果；如仍需要，请重新调用。";
    return result;
}

// Old archives may end halfway through a tool batch after interruption or exit.
// Keep completed results, close missing calls, and discard unmatched results.
inline std::vector<Msg> repairToolHistory(std::vector<Msg> messages) {
    std::vector<Msg> repaired;
    repaired.reserve(messages.size());
    std::vector<std::string> pending;
    std::set<std::string> received;
    const auto finishPending = [&] {
        for (const auto& id : pending)
            if (!received.count(id)) repaired.push_back(cancelledToolResult(id));
        pending.clear();
        received.clear();
    };
    for (auto& msg : messages) {
        if (msg.role == Role::Tool) {
            if (std::find(pending.begin(), pending.end(), msg.toolCallId) != pending.end() &&
                received.insert(msg.toolCallId).second)
                repaired.push_back(std::move(msg));
            continue;
        }
        finishPending();
        if (msg.role == Role::Assistant)
            for (const auto& call : msg.toolCalls) pending.push_back(call.id);
        repaired.push_back(std::move(msg));
    }
    finishPending();
    return repaired;
}

// A tool-call assistant has not answered the user's pending input yet.
inline std::size_t pendingTurnStart(const std::vector<Msg>& messages) {
    for (std::size_t i = messages.size(); i > 0; --i)
        if (messages[i - 1].role == Role::Assistant && messages[i - 1].toolCalls.empty())
            return i;
    return 0;
}

inline std::size_t safeTailStart(const std::vector<Msg>& messages, int rawKeep) {
    const auto keep = static_cast<std::size_t>(std::max(rawKeep, 0));
    std::size_t start = messages.size() > keep ? messages.size() - keep : 0;
    start = std::min(start, pendingTurnStart(messages));
    // Never begin with results whose assistant tool_calls have been removed.
    while (start > 0 && start < messages.size() && messages[start].role == Role::Tool)
        --start;
    return start;
}

inline void stripCompletedMedia(std::vector<Msg>& messages) {
    const auto end = pendingTurnStart(messages);
    for (std::size_t i = 0; i < end; ++i) {
        messages[i].parts.clear();
    }
}

} // namespace mio::history
