#include "adapters/console/ConsoleAdapter.h"

#include <chrono>
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
           << "/reload [path]           热重载配置（默认读取 config.json，不存在则回退环境变量）\n"
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
           << "activeChannel: ";

    if (state.activeChannel.has_value()) {
        output << *state.activeChannel;
    } else {
        output << "none";
    }

    output << "\nactiveUsers: " << state.activeUsers.size()
           << "\n-----------------\n";
}

bool parseEvent(const std::string& input, Event& ev) {
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
        std::string text;
        std::getline(stream, text);
        if (!text.empty() && text.front() == ' ') {
            text.erase(0, 1);
        }
        if (text.empty()) {
            return false;
        }
        ev.category = EventCategory::ExternalMessage;
        ev.tier = AttentionTier::DirectFull;
        ev.source = "console";
        ev.channelId = "private:0d00";
        ev.actorId = "0d00";
        ev.title = "0d00";
        ev.content = text;
        ev.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
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

    if (scope != "private" && scope != "group") {
        return false;
    }

    ev.category = EventCategory::ExternalMessage;
    ev.tier = AttentionTier::DirectFull;
    ev.source = "console";
    ev.channelId = scope + ":" + conversationId;
    ev.actorId = senderId;
    ev.title = senderId;
    ev.content = text;
    ev.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    return true;
}

} // namespace

void runConsole(Runtime& runtime, std::istream& input, std::ostream& output) {
    runtime.setOutboundSender([&output](const Outbound& out) -> bool {
        output << "[" << out.channelId << "] " << out.text << "\n";
        output.flush();
        return true;
    });

    printHelp(output);

    std::string line;
    while (std::getline(input, line)) {
        if (line == "/quit") {
            output << "已退出。\n";
            return;
        }

        if (line == "/help") {
            printHelp(output);
        } else if (line == "/state") {
            printState(runtime, output);
        } else if (line.rfind("/reload", 0) == 0) {
            std::istringstream stream(line);
            std::string cmd;
            std::string path = "config.json";
            stream >> cmd;
            if (stream >> path) {
                // user provided path
            }
            if (runtime.reloadConfig(path)) {
                output << "[Config] 热重载成功 (配置来源: " << path << ")\n";
            } else {
                output << "[Config] 热重载失败，已保持现有配置运行。\n";
            }
        } else {
            Event ev;
            if (parseEvent(line, ev)) {
                try {
                    const std::string cid = ev.channelId;
                    runtime.postEvent(std::move(ev));
                    const BotReply reply = runtime.flushMindLoop();
                    if (reply.text.empty() && !reply.interrupted) {
                        output << "[" << cid << "] (保持沉默/已读不回)\n";
                    }
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
