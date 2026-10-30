#include "mio/plugin.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

class ConsolePlugin final : public mio::sdk::Plugin, public mio::sdk::Channel {
public:
    ConsolePlugin(mio::sdk::Host& host, const nlohmann::json& /*cfg*/)
        : host_(host) {
        host_.registerChannel("console", this);
    }

    ~ConsolePlugin() override {
        stop();
    }

    bool start() override {
        if (running_.exchange(true)) return true;
        worker_ = std::thread(&ConsolePlugin::loop, this);
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                worker_.detach();
            } else {
                // stdin getline 可能阻塞在交互终端，进程退出时允许 detach
                worker_.detach();
            }
        }
    }

    bool send(const nlohmann::json& outbound) override {
        const std::string cid = outbound.value("channelId", "console");
        const std::string text = outbound.value("text", "");
        std::cout << "[" << cid << "] " << text << "\n";
        std::cout.flush();
        return true;
    }

    nlohmann::json capabilities() const override {
        return {{"reply", false}, {"mention", false}, {"maxLen", 8000}};
    }

private:
    static void printHelp() {
        std::cout << "Mio 控制台演示\n"
                  << "/msg <private|group> <conversation_id> <sender_id> <text>\n"
                  << "/chat <text>             简易私聊：固定发给用户 0d00\n"
                  << "/reload [path]           热重载配置（默认读取 config.json）\n"
                  << "/state\n"
                  << "/help\n"
                  << "/quit\n\n";
    }

    void printState() const {
        const auto st = host_.stateJson();
        std::cout << "--- RuntimeState ---\n"
                  << "messageCount: " << st.value("messageCount", 0ull) << "\n"
                  << "botName: " << st.value("botName", "Mio") << "\n"
                  << "version: " << st.value("version", 0ull) << "\n"
                  << "lastEventSeq: " << st.value("lastEventSeq", 0ull) << "\n"
                  << "activeChannel: " << st.value("activeChannel", "none") << "\n"
                  << "activeUsers: " << st.value("activeUsersCount", 0ull) << "\n"
                  << "-----------------\n";
    }

    static bool parseEvent(const std::string& input, nlohmann::json& ev) {
        std::istringstream stream(input);
        std::string command;
        std::string scope;
        std::string conversationId;
        std::string senderId;

        if (!(stream >> command)) return false;

        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();

        if (command == "/chat") {
            std::string text;
            std::getline(stream, text);
            if (!text.empty() && text.front() == ' ') text.erase(0, 1);
            if (text.empty()) return false;
            ev = {
                {"category", "ExternalMessage"},
                {"tier", "DirectFull"},
                {"source", "console"},
                {"channelId", "private:0d00"},
                {"actorId", "0d00"},
                {"title", "0d00"},
                {"content", text},
                {"timestamp", nowMs}
            };
            return true;
        }

        if (command != "/msg") return false;
        if (!(stream >> scope >> conversationId >> senderId)) return false;
        if (scope != "private" && scope != "group") return false;

        std::string text;
        std::getline(stream, text);
        if (!text.empty() && text.front() == ' ') text.erase(0, 1);
        if (text.empty()) return false;

        ev = {
            {"category", "ExternalMessage"},
            {"tier", "DirectFull"},
            {"source", "console"},
            {"channelId", scope + ":" + conversationId},
            {"actorId", senderId},
            {"title", senderId},
            {"content", text},
            {"timestamp", nowMs}
        };
        return true;
    }

    void loop() {
        printHelp();
        std::cout << "> ";
        std::cout.flush();

        std::string line;
        while (running_.load() && std::getline(std::cin, line)) {
            if (!running_.load()) break;

            if (line == "/quit") {
                std::cout << "已退出。\n";
                host_.requestShutdown();
                return;
            }

            if (line == "/help") {
                printHelp();
            } else if (line == "/state") {
                printState();
            } else if (line.rfind("/reload", 0) == 0) {
                std::istringstream stream(line);
                std::string cmd;
                std::string path = "config.json";
                stream >> cmd;
                stream >> path;
                if (host_.reloadConfig(path)) {
                    std::cout << "[Config] 热重载成功 (配置来源: " << path << ")\n";
                } else {
                    std::cout << "[Config] 热重载失败，已保持现有配置运行。\n";
                }
            } else {
                nlohmann::json ev;
                if (parseEvent(line, ev)) {
                    const std::string cid = ev.value("channelId", "console");
                    if (host_.postEvent(ev)) {
                        const auto reply = host_.flushMindLoop();
                        if (reply.value("text", "").empty() && !reply.value("interrupted", false)) {
                            std::cout << "[" << cid << "] (保持沉默/已读不回)\n";
                        }
                    }
                } else if (!line.empty()) {
                    std::cout << "无法识别的命令，请输入 /help。\n";
                }
            }

            std::cout << "> ";
            std::cout.flush();
        }
        if (running_.load()) {
            // EOF on stdin
            host_.requestShutdown();
        }
    }

    mio::sdk::Host& host_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace

MIO_PLUGIN(ConsolePlugin, "console", "0.1.0")
