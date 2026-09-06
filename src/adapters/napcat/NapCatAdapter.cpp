// ============================================================================
// NapCatQQ 适配器实现（反向 WebSocket）
//
// 模块边界：
//   1. OneBotTypes / OneBotParser —— 事件 JSON -> 结构化 OneBotEvent / IncomingMessage；
//      只做解析，不碰网络与 Runtime。
//   2. 本文件 —— WebSocket 服务端、鉴权、action 下发/echo 匹配、消息队列、
//      后台 worker 调度 Runtime::ingest。
//
// 事件处理策略：
//   * message 事件：解析成 IncomingMessage 后入队交给 Runtime；
//   * notice / request / meta_event：当前 Runtime 暂不消费，但会完整解析为
//     OneBotEvent 并输出 Info 日志，结构化字段已保留供后续扩展。
// ============================================================================

#include "adapters/napcat/NapCatAdapter.h"
#include "adapters/napcat/OneBotParser.h"
#include "runtime/Runtime.h"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "log/Log.h"

namespace mio {

namespace {

constexpr const char* kTag = "NapCat";
constexpr int kActionTimeoutSec = 30;

// --- 环境变量（与 OpenaiConfig.cpp 同款）------------------------------------
std::optional<std::string> env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

std::optional<int> envInt(const char* name) {
    if (auto v = env(name)) {
        try {
            return std::stoi(*v);
        } catch (...) {
        }
    }
    return std::nullopt;
}

// QQ 号/群号发送时尽量按数字发送，部分 NapCat 版本只接受数字
nlohmann::json idToJson(const std::string& id) {
    try {
        std::size_t consumed = 0;
        const unsigned long long n = std::stoull(id, &consumed);
        if (consumed == id.size() && !id.empty()) return n;
    } catch (...) {
    }
    return id;
}

// --- 连入鉴权 -------------------------------------------------------------------
std::string queryParam(const std::string& uri, const std::string& key) {
    const std::size_t question = uri.find('?');
    if (question == std::string::npos) return "";
    std::istringstream stream(uri.substr(question + 1));
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            std::string value = pair.substr(eq + 1);
            // Access tokens are commonly URL-encoded when supplied in the
            // reverse-WebSocket URL. Decode the small subset needed here.
            std::string decoded;
            decoded.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '%' && i + 2 < value.size()) {
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int hi = hex(value[i + 1]);
                    const int lo = hex(value[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        decoded.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                decoded.push_back(value[i] == '+' ? ' ' : value[i]);
            }
            return decoded;
        }
    }
    return "";
}

bool handshakeAuthorized(const std::string& uri,
                         const ix::WebSocketHttpHeaders& headers,
                         const NapCatConfig& cfg) {
    if (cfg.token.empty()) return true;
    if (queryParam(uri, "access_token") == cfg.token) return true;
    for (const auto& header : headers) {
        std::string name = header.first;
        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name == "authorization" && header.second == "Bearer " + cfg.token)
            return true;
    }
    return false;
}

// --- action 帧（echo 匹配）-------------------------------------------------------
struct PendingActions {
    std::mutex mtx;
    std::map<std::string, std::shared_ptr<std::promise<nlohmann::json>>> map;
    std::uint64_t seq = 0;
};

void resolveAction(PendingActions& pending, const nlohmann::json& resp) {
    std::shared_ptr<std::promise<nlohmann::json>> promise;
    {
        std::lock_guard<std::mutex> lock(pending.mtx);
        const auto it = pending.map.find(resp.value("echo", ""));
        if (it == pending.map.end()) return;  // 超时清理后的迟到响应，丢弃
        promise = it->second;
        pending.map.erase(it);
    }
    try {
        promise->set_value(resp);
    } catch (const std::exception&) {
    }
}

void failPending(PendingActions& pending) {
    std::vector<std::shared_ptr<std::promise<nlohmann::json>>> promises;
    {
        std::lock_guard<std::mutex> lock(pending.mtx);
        for (auto& item : pending.map) promises.push_back(item.second);
        pending.map.clear();
    }
    for (const auto& promise : promises) {
        try {
            promise->set_value(nlohmann::json{{"status", "failed"},
                                              {"retcode", -1}});
        } catch (const std::exception&) {
        }
    }
}

bool callAction(const std::shared_ptr<ix::WebSocket>& ws, PendingActions& pending,
                const std::string& action, nlohmann::json params) {
    if (!ws) {
        log::error(kTag, action + " 失败: NapCat 未连接");
        return false;
    }

    std::string echo;
    std::shared_ptr<std::promise<nlohmann::json>> promise;
    {
        std::lock_guard<std::mutex> lock(pending.mtx);
        echo = "mio-" + std::to_string(++pending.seq);
        promise = std::make_shared<std::promise<nlohmann::json>>();
        pending.map.emplace(echo, promise);
    }

    const auto info =
        ws->sendText(nlohmann::json{{"action", action},
                                    {"params", std::move(params)},
                                    {"echo", echo}}
                         .dump());
    log::debug(kTag, "action " + action + " echo=" + echo + " 发送" +
                         (info.success ? "成功" : "失败"));

    if (!info.success) {
        std::lock_guard<std::mutex> lock(pending.mtx);
        pending.map.erase(echo);
        log::error(kTag, action + " 发送失败");
        return false;
    }

    std::future<nlohmann::json> future = promise->get_future();
    if (future.wait_for(std::chrono::seconds(kActionTimeoutSec)) !=
        std::future_status::ready) {
        std::lock_guard<std::mutex> lock(pending.mtx);
        pending.map.erase(echo);
        log::error(kTag, action + " 超时未收到响应");
        return false;
    }

    try {
        const auto response = future.get();
        const bool okStatus = !response.contains("status") ||
                              response["status"] == "ok";
        const int retcode = response.contains("retcode")
                                ? (response["retcode"].is_string()
                                       ? std::stoi(response["retcode"].get<std::string>())
                                       : response["retcode"].get<int>())
                                : -1;
        if (okStatus && retcode == 0) return true;
    } catch (const std::exception&) {
    }
    log::error(kTag, action + " 返回异常");
    return false;
}

bool sendReply(const std::shared_ptr<ix::WebSocket>& ws, PendingActions& pending,
               const ConversationKey& conversation, const std::string& senderId,
               const std::string& text) {
    nlohmann::json params{{"message", text}};
    if (conversation.scope == ConversationScope::Group) {
        // NapCat's documented unified API is send_msg. It accepts the same
        // payload as send_group_msg while making the target type explicit.
        params["message_type"] = "group";
        params["group_id"] = idToJson(conversation.id);
    } else {
        params["message_type"] = "private";
        params["user_id"] = idToJson(senderId);
    }
    return callAction(ws, pending, "send_msg", std::move(params));
}

// --- 连接与 worker ---------------------------------------------------------------
struct NapCatConnection {
    std::mutex mtx;
    std::shared_ptr<ix::WebSocket> current;
};

void workerLoop(Runtime& runtime, NapCatConnection& conn, PendingActions& pending,
                std::deque<IncomingMessage>& queue, std::mutex& queueMtx,
                std::condition_variable& queueCv, const std::atomic<bool>& running) {
    for (;;) {
        std::unique_lock<std::mutex> lock(queueMtx);
        queueCv.wait(lock, [&] { return !queue.empty() || !running; });
        if (queue.empty()) return;
        IncomingMessage message = std::move(queue.front());
        queue.pop_front();
        lock.unlock();

        const ConversationKey conversation = message.conversation;
        const std::string senderId = message.senderId;
        log::info(kTag, "处理 " + conversation.toString() + " 来自 " + senderId);

        try {
            const BotReply reply = runtime.ingest(std::move(message));
            if (reply.text.empty()) continue;

            std::shared_ptr<ix::WebSocket> ws;
            {
                std::lock_guard<std::mutex> connLock(conn.mtx);
                ws = conn.current;
            }
            if (sendReply(ws, pending, conversation, senderId, reply.text))
                log::info(kTag, "已回复 → " + conversation.toString());
        } catch (const std::exception& error) {
            log::error(kTag, std::string("处理消息失败: ") + error.what());
        }
    }
}

} // namespace

// --- 配置 ------------------------------------------------------------------------
NapCatConfig NapCatConfig::fromEnvironment() {
    NapCatConfig cfg;
    if (auto v = env("MIO_NAPCAT_HOST")) cfg.listenHost = *v;
    if (auto v = envInt("MIO_NAPCAT_PORT")) cfg.listenPort = *v;
    if (auto v = env("MIO_NAPCAT_TOKEN")) cfg.token = *v;
    return cfg;
}

void runNapCat(Runtime& runtime, const NapCatConfig& config) {
    PendingActions pending;
    NapCatConnection conn;
    std::deque<IncomingMessage> queue;
    std::mutex queueMtx;
    std::condition_variable queueCv;
    std::atomic<bool> running{true};

    std::thread worker(workerLoop, std::ref(runtime), std::ref(conn),
                       std::ref(pending), std::ref(queue), std::ref(queueMtx),
                       std::ref(queueCv), std::cref(running));
    auto stopWorker = [&] {
        running = false;
        queueCv.notify_all();
        worker.join();
    };

    ix::WebSocketServer server(config.listenPort, config.listenHost);
    server.enablePong();
    server.setOnConnectionCallback(
        [&](std::weak_ptr<ix::WebSocket> wsock,
            std::shared_ptr<ix::ConnectionState> /*state*/) {
            const auto ws = wsock.lock();
            if (!ws) return;

            ws->setOnMessageCallback(
                [&config, &conn, &pending, &queue, &queueMtx, &queueCv, ws](
                    const ix::WebSocketMessagePtr& msg) {
                    switch (msg->type) {
                    case ix::WebSocketMessageType::Open: {
                        if (!handshakeAuthorized(msg->openInfo.uri,
                                                 msg->openInfo.headers,
                                                 config)) {
                            log::warn(kTag, "拒绝未授权的 NapCat 连接");
                            ws->close(1008, "invalid access_token");
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(conn.mtx);
                            conn.current = ws;
                        }
                        log::info(kTag, "NapCat 已连入");
                        break;
                    }
                    case ix::WebSocketMessageType::Message: {
                        try {
                            const nlohmann::json frame =
                                nlohmann::json::parse(msg->str);

                            // action 响应：带 echo 且没有 post_type
                            if (frame.contains("echo") &&
                                !frame.contains("post_type")) {
                                resolveAction(pending, frame);
                                break;
                            }

                            const auto ev = napcat::parseOneBotEvent(frame);
                            switch (ev.postType) {
                            case napcat::OneBotPostType::Message:
                                if (auto message =
                                        napcat::eventToIncomingMessage(ev)) {
                                    std::lock_guard<std::mutex> lock(queueMtx);
                                    queue.push_back(std::move(*message));
                                    queueCv.notify_one();
                                }
                                break;
                            case napcat::OneBotPostType::MetaEvent:
                                if (ev.eventType == "lifecycle") {
                                    log::info(kTag,
                                              "NapCat 握手完成 self_id=" +
                                                  ev.selfId);
                                } else {
                                    log::debug(kTag,
                                               "meta_event: " +
                                                   napcat::describeOneBotEvent(ev));
                                }
                                break;
                            case napcat::OneBotPostType::Notice:
                            case napcat::OneBotPostType::Request:
                                // 当前 Runtime 不消费 notice/request；
                                // 已完整解析为 OneBotEvent，先 Info 记录现场。
                                log::info(kTag,
                                          "收到 " +
                                              std::string(
                                                  ev.postTypeName == "notice"
                                                      ? "通知"
                                                      : "请求") +
                                              "事件: " +
                                              napcat::describeOneBotEvent(ev));
                                break;
                            case napcat::OneBotPostType::Unknown:
                                log::warn(kTag,
                                          "无法识别的事件帧: " + msg->str);
                                break;
                            }
                        } catch (const std::exception& error) {
                            log::warn(kTag,
                                      std::string("帧解析失败: ") + error.what());
                        }
                        break;
                    }
                    case ix::WebSocketMessageType::Close: {
                        std::lock_guard<std::mutex> lock(conn.mtx);
                        if (conn.current == ws) conn.current.reset();
                        failPending(pending);
                        log::info(kTag, "NapCat 连接断开");
                        break;
                    }
                    default:
                        break;  // Ping 由 ix 自动回 Pong；Error 随 Close 事件
                    }
                });
        });

    log::info(kTag, "反向 WS 监听 " + config.listenHost + ":" +
                        std::to_string(config.listenPort) +
                        (config.token.empty() ? "（未鉴权）" : "（token 鉴权）") +
                        "，等待 NapCat 连入…");

    auto bound = server.listen();
    for (int attempt = 2; !bound.first && attempt <= 3; ++attempt) {
        log::warn(kTag, "绑定 " + config.listenHost + ":" +
                            std::to_string(config.listenPort) +
                            " 失败，2 秒后重试（" + std::to_string(attempt) +
                            "/3）");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        bound = server.listen();
    }
    if (!bound.first) {
        stopWorker();
        throw std::runtime_error(
            "反向 WS 端口监听失败: " + config.listenHost + ":" +
            std::to_string(config.listenPort) + " (" + bound.second +
            ")。MIO_NAPCAT_HOST 是 MIO 本机的监听地址（不是 NapCat 的地址），"
            "必须已配置在本机某个网卡上；NapCat 在本机时保持默认 127.0.0.1，"
            "跨机部署可用 0.0.0.0 监听全部网卡。用 ip addr 可查看本机地址。");
    }
    server.start();
    server.wait();
    stopWorker();
}

} // namespace mio
