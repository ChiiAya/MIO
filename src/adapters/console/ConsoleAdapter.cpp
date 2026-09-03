#include "adapters/console/ConsoleAdapter.h"

#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mio {
namespace {

void printHelp(std::ostream& output) {
    output << "Mio 控制台演示\n"
           << "/msg <private|group> <conversation_id> <sender_id> <text>\n"
           << "/chat <text>             简易私聊：固定发给用户 0d00\n"
           << "/state\n"
           << "/help\n"
           << "/quit\n\n";
}

void printState(const Runtime& runtime, std::ostream& output) {
    const RuntimeState& state = runtime.state();
    output << "--- RuntimeState ---\n"
           << "messageCount: " << state.messageCount << "\n"
           << "botName: " << state.botName << "\n"
           << "version: " << state.version << "\n"
           << "lastEventSeq: " << state.lastEventSeq << "\n"
           << "activeConversation: ";

    if (state.activeConversation.has_value()) {
        output << state.activeConversation->toString();
    } else {
        output << "none";
    }

    output << "\nactiveUsers: " << state.activeUsers.size()
           << "\n-----------------\n";
}

bool parseMessage(const std::string& input, IncomingMessage& message) {
    std::istringstream stream(input);
    std::string command;
    std::string scope;
    std::string conversationId;
    std::string senderId;

    if (!(stream >> command)) {
        return false;
    }

    if (command == "/msg") {
        if (!(stream >> scope >> conversationId >> senderId)) {
            return false;
        }
    } else if (command == "/chat") {
        // 简易私聊：固定发给用户 0d00，免拼会话参数
        std::string text;
        std::getline(stream, text);
        if (!text.empty() && text.front() == ' ') {
            text.erase(0, 1);
        }
        if (text.empty()) {
            return false;
        }
        message = {ConversationKey::privateChat("0d00"), "0d00", "", text,
                   "console"};
        return true;
    } else {
        return false;
    }

    std::string text;
    std::getline(stream, text);
    if (!text.empty() && text.front() == ' ') {
        text.erase(0, 1);
    }

    if (text.empty()) {
        return false;
    }

    ConversationKey conversation;
    if (scope == "private") {
        conversation = ConversationKey::privateChat(conversationId);
    } else if (scope == "group") {
        conversation = ConversationKey::groupChat(conversationId);
    } else {
        return false;
    }

    message = {
        conversation,
        senderId,
        scope == "group" ? conversationId : "",  // groupId：群聊时填，私聊留空
        text,
        "console",
    };
    return true;
}

} // namespace

void runConsole(Runtime& runtime, std::istream& input, std::ostream& output) {
    printHelp(output);

    std::string line;
    while (std::getline(input, line)) {
        if (line == "/quit") {
            output << "已退出。事件状态尚未持久化，这是当前阶段的预期行为。\n";
            return;
        }

        if (line == "/help") {
            printHelp(output);
        } else if (line == "/state") {
            printState(runtime, output);
        } else {
            IncomingMessage message;
            if (parseMessage(line, message)) {
                try {
                    const BotReply reply = runtime.ingest(std::move(message));
                    output << "[" << reply.conversation.toString() << "] " << reply.text << "\n";
                } catch (const std::exception& error) {
                    output << "[LLM error] " << error.what() << "\n";
                }
            } else if (!line.empty()) {
                output << "无法识别的命令，请输入 /help。\n";
            }
        }

        output << "> ";
    }
}

} // namespace mio
