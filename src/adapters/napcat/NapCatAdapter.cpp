#include "adapters/napcat/OneBotMessage.h"
// ============================================================================
// NapCatQQ 适配器实现（反向 WebSocket）
// ============================================================================

#include "adapters/napcat/NapCatAdapter.h"
#include "adapters/napcat/MediaResolver.h"
#include "adapters/napcat/OneBotParser.h"
#include "providers/asr/openai/OpenAiAsr.h"
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

std::string queryParam(const std::string& uri, const std::string& key) {
    const std::size_t question = uri.find('?');
    if (question == std::string::npos) return "";
    std::istringstream stream(uri.substr(question + 1));
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            std::string value = pair.substr(eq + 1);
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
        if (it == pending.map.end()) return;
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
               const Outbound& outbound) {
    return callAction(ws, pending, "send_msg", napcat::buildSendParams(outbound));
}

struct NapCatConnection {
    std::mutex mtx;
    std::shared_ptr<ix::WebSocket> current;
};

struct QueuedOneBotItem {
    Event mioEvent;
    std::vector<MediaAttachment> attachments;
};

void workerLoop(Runtime& runtime, const MediaResolver& mediaResolver,
                std::deque<QueuedOneBotItem>& queue, std::mutex& queueMtx,
                std::condition_variable& queueCv, const std::atomic<bool>& running) {
    for (;;) {
        std::unique_lock<std::mutex> lock(queueMtx);
        queueCv.wait(lock, [&] { return !queue.empty() || !running; });
        if (queue.empty()) {
            return;
        }
        QueuedOneBotItem item = std::move(queue.front());
        queue.pop_front();
        lock.unlock();

        try {
            if (!item.attachments.empty()) {
                mediaResolver.resolve(item.attachments, item.mioEvent);
                log::info(kTag, "媒体解析完成 count=" +
                                    std::to_string(item.attachments.size()) +
                                    " parts=" +
                                    std::to_string(item.mioEvent.parts.size()));
            }
            runtime.postEvent(std::move(item.mioEvent));
        } catch (const std::exception& error) {
            log::error(kTag, std::string("投递事件失败: ") + error.what());
        }
    }
}

} // namespace

NapCatConfig NapCatConfig::fromEnvironment() {
    NapCatConfig cfg;
    if (auto v = env("MIO_NAPCAT_HOST")) cfg.listenHost = *v;
    if (auto v = envInt("MIO_NAPCAT_PORT")) cfg.listenPort = *v;
    if (auto v = env("MIO_NAPCAT_TOKEN")) cfg.token = *v;
    cfg.media = MediaConfig::fromEnvironment();
    return cfg;
}

void runNapCat(Runtime& runtime, const NapCatConfig& config) {
    PendingActions pending;
    NapCatConnection conn;
    std::shared_ptr<AsrProvider> asr;
    if (config.media.enabled && config.media.audio && config.media.asr.enabled) {
        asr = std::make_shared<OpenAiAsr>(config.media.asr);
    }
    const MediaResolver mediaResolver(config.media, std::move(asr));

    std::deque<QueuedOneBotItem> queue;
    std::mutex queueMtx;
    std::condition_variable queueCv;
    std::atomic<bool> running{true};

    runtime.setOutboundSender([&conn, &pending](const Outbound& outbound) -> bool {
        std::shared_ptr<ix::WebSocket> ws;
        {
            std::lock_guard<std::mutex> connLock(conn.mtx);
            ws = conn.current;
        }
        if (!ws) return false;
        return sendReply(ws, pending, outbound);
    });

    std::thread worker(workerLoop, std::ref(runtime),
                       std::cref(mediaResolver),
                       std::ref(queue), std::ref(queueMtx), std::ref(queueCv),
                       std::cref(running));
    auto stopWorker = [&] {
        running = false;
        queueCv.notify_all();
        if (worker.joinable()) worker.join();
    };

    ix::WebSocketServer server(config.listenPort, config.listenHost);
    server.enablePong();
    server.setOnConnectionCallback(
        [&](std::weak_ptr<ix::WebSocket> wsock,
            std::shared_ptr<ix::ConnectionState>) {
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

                            if (frame.contains("echo") &&
                                !frame.contains("post_type")) {
                                resolveAction(pending, frame);
                                break;
                            }

                            const auto ev = napcat::parseOneBotEvent(frame);
                            switch (ev.postType) {
                            case napcat::OneBotPostType::Message:
                            case napcat::OneBotPostType::Notice:
                            case napcat::OneBotPostType::Request: {
                                if (auto mioEv = napcat::oneBotEventToMioEvent(ev)) {
                                    QueuedOneBotItem item;
                                    item.mioEvent = std::move(*mioEv);
                                    item.attachments = ev.attachments;
                                    std::lock_guard<std::mutex> lock(queueMtx);
                                    queue.push_back(std::move(item));
                                    queueCv.notify_one();
                                }
                                break;
                            }
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
                        break;
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
            std::to_string(config.listenPort) + " (" + bound.second + ")");
    }
    server.start();
    server.wait();
    stopWorker();
}

} // namespace mio
