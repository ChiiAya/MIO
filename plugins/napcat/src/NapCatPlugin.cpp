#include "mio/plugin.hpp"

#include "adapters/napcat/MediaResolver.h"
#include "adapters/napcat/NapCatAdapter.h"
#include "adapters/napcat/OneBotMessage.h"
#include "adapters/napcat/OneBotParser.h"
#include "config/AppConfig.h"
#include "providers/asr/openai/OpenAiAsr.h"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

struct QueuedOneBotItem {
    Event mioEvent;
    std::vector<MediaAttachment> attachments;
};

} // namespace

NapCatConfig NapCatConfig::fromEnvironment() {
    NapCatConfig cfg;
    if (auto v = env("MIO_NAPCAT_HOST")) cfg.listenHost = *v;
    if (auto v = envInt("MIO_NAPCAT_PORT")) cfg.listenPort = *v;
    if (auto v = env("MIO_NAPCAT_TOKEN")) cfg.token = *v;
    cfg.media = MediaConfig::fromEnvironment();
    return cfg;
}

void to_json(nlohmann::json& j, const NapCatConfig& c) {
    j = nlohmann::json{
        {"listenHost", c.listenHost},
        {"listenPort", c.listenPort},
        {"token", c.token},
        {"media", c.media}
    };
}

void from_json(const nlohmann::json& j, NapCatConfig& c) {
    if (j.contains("listenHost") && j["listenHost"].is_string())
        c.listenHost = j["listenHost"].get<std::string>();
    if (j.contains("listenPort") && j["listenPort"].is_number_integer())
        c.listenPort = j["listenPort"].get<int>();
    if (j.contains("token") && j["token"].is_string())
        c.token = j["token"].get<std::string>();
    if (j.contains("media") && j["media"].is_object())
        from_json(j["media"], c.media);
}

namespace {

class NapCatPlugin final : public sdk::Plugin, public sdk::Channel {
public:
    NapCatPlugin(sdk::Host& host, const nlohmann::json& cfgJson)
        : host_(host), config_(NapCatConfig::fromEnvironment()) {
        if (cfgJson.is_object() && !cfgJson.empty()) {
            from_json(cfgJson, config_);
        }
        std::shared_ptr<AsrProvider> asr;
        if (config_.media.enabled && config_.media.audio && config_.media.asr.enabled) {
            asr = std::make_shared<OpenAiAsr>(config_.media.asr);
        }
        mediaResolver_ = std::make_unique<MediaResolver>(config_.media, std::move(asr));
        host_.registerChannel("napcat", this);

        nlohmann::json toolDef = {
            {"name", "get_current_user_qq"},
            {"description", "获取最近一次在 QQ/NapCat 发言的用户的 QQ 号、昵称及会话信息（如私聊或群号）。"},
            {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}}
        };
        host_.registerTool(toolDef, [this](const nlohmann::json&) -> std::string {
            std::lock_guard<std::mutex> lock(lastUserMtx_);
            if (lastUserId_.empty() && lastChannelId_.empty()) {
                return "error: 当前暂无可用的 QQ 对话上下文";
            }
            nlohmann::json info;
            info["platform"] = "napcat";
            info["user_id"] = lastUserId_;
            info["qq"] = lastUserId_;
            info["nickname"] = lastNickname_;
            info["channel_id"] = lastChannelId_;
            const bool isGroup = !lastGroupId_.empty();
            info["scope"] = isGroup ? "group" : "private";
            if (isGroup) {
                info["group_id"] = lastGroupId_;
            }
            return info.dump();
        });
    }

    ~NapCatPlugin() override {
        stop();
    }

    bool start() override {
        if (running_.exchange(true)) return true;

        worker_ = std::thread(&NapCatPlugin::workerLoop, this);

        server_ = std::make_unique<ix::WebSocketServer>(config_.listenPort, config_.listenHost);
        server_->enablePong();
        server_->setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> wsock,
                   std::shared_ptr<ix::ConnectionState>) {
                const auto ws = wsock.lock();
                if (!ws) return;

                ws->setOnMessageCallback(
                    [this, ws](const ix::WebSocketMessagePtr& msg) {
                        onWsMessage(ws, msg);
                    });
            });

        host_.info(kTag, "反向 WS 监听 " + config_.listenHost + ":" +
                             std::to_string(config_.listenPort) +
                             (config_.token.empty() ? "（未鉴权）" : "（token 鉴权）") +
                             "，等待 NapCat 连入…");

        auto bound = server_->listen();
        for (int attempt = 2; !bound.first && attempt <= 3; ++attempt) {
            host_.warn(kTag, "绑定 " + config_.listenHost + ":" +
                                 std::to_string(config_.listenPort) +
                                 " 失败，2 秒后重试（" + std::to_string(attempt) +
                                 "/3）");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            bound = server_->listen();
        }
        if (!bound.first) {
            running_.store(false);
            queueCv_.notify_all();
            if (worker_.joinable()) worker_.join();
            server_.reset();
            host_.error(kTag, "反向 WS 端口监听失败: " + config_.listenHost + ":" +
                                  std::to_string(config_.listenPort) + " (" + bound.second + ")");
            return false;
        }
        server_->start();
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) return;
        queueCv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (server_) {
            server_->stop();
            server_.reset();
        }
        failPending(pending_);
    }

    bool send(const nlohmann::json& outboundJson) override {
        Outbound outbound;
        outbound.source = outboundJson.value("source", "napcat");
        outbound.channelId = outboundJson.value("channelId", "");
        outbound.text = outboundJson.value("text", "");
        outbound.replyTo = outboundJson.value("replyTo", "");
        if (outboundJson.contains("mentions") && outboundJson["mentions"].is_array()) {
            for (const auto& m : outboundJson["mentions"]) {
                if (m.is_string()) outbound.mentions.push_back(m.get<std::string>());
            }
        }
        if (outboundJson.contains("parts") && outboundJson["parts"].is_array()) {
            for (const auto& item : outboundJson["parts"]) {
                outbound.parts.push_back(Part{
                    partKindFromString(item.value("kind", "image")),
                    item.value("data", ""),
                    item.value("mimeType", "")
                });
            }
        }

        std::shared_ptr<ix::WebSocket> ws;
        {
            std::lock_guard<std::mutex> lock(connMtx_);
            ws = currentWs_;
        }
        if (!ws) {
            host_.error(kTag, "send_msg 失败: NapCat 未连接");
            return false;
        }
        return callAction(ws, "send_msg", napcat::buildSendParams(outbound));
    }

    nlohmann::json capabilities() const override {
        return {{"reply", true}, {"mention", true}, {"maxLen", 4500}};
    }

private:
    bool callAction(const std::shared_ptr<ix::WebSocket>& ws,
                    const std::string& action,
                    nlohmann::json params) {
        std::string echo;
        std::shared_ptr<std::promise<nlohmann::json>> promise;
        {
            std::lock_guard<std::mutex> lock(pending_.mtx);
            echo = "mio-" + std::to_string(++pending_.seq);
            promise = std::make_shared<std::promise<nlohmann::json>>();
            pending_.map.emplace(echo, promise);
        }

        const auto info =
            ws->sendText(nlohmann::json{{"action", action},
                                        {"params", std::move(params)},
                                        {"echo", echo}}
                             .dump());
        if (!info.success) {
            std::lock_guard<std::mutex> lock(pending_.mtx);
            pending_.map.erase(echo);
            host_.error(kTag, action + " 发送失败");
            return false;
        }

        std::future<nlohmann::json> future = promise->get_future();
        if (future.wait_for(std::chrono::seconds(kActionTimeoutSec)) !=
            std::future_status::ready) {
            std::lock_guard<std::mutex> lock(pending_.mtx);
            pending_.map.erase(echo);
            host_.error(kTag, action + " 超时未收到响应");
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
        host_.error(kTag, action + " 返回异常");
        return false;
    }

    void onWsMessage(const std::shared_ptr<ix::WebSocket>& ws,
                     const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            if (!handshakeAuthorized(msg->openInfo.uri, msg->openInfo.headers, config_)) {
                host_.warn(kTag, "拒绝未授权的 NapCat 连接");
                ws->close(1008, "invalid access_token");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(connMtx_);
                currentWs_ = ws;
            }
            host_.info(kTag, "NapCat 已连入");
            break;
        }
        case ix::WebSocketMessageType::Message: {
            try {
                const nlohmann::json frame = nlohmann::json::parse(msg->str);
                if (frame.contains("echo") && !frame.contains("post_type")) {
                    resolveAction(pending_, frame);
                    break;
                }

                const auto ev = napcat::parseOneBotEvent(frame);
                switch (ev.postType) {
                case napcat::OneBotPostType::Message:
                case napcat::OneBotPostType::Notice:
                case napcat::OneBotPostType::Request: {
                    if (auto mioEv = napcat::oneBotEventToMioEvent(ev)) {
                        if (ev.postType == napcat::OneBotPostType::Message) {
                            std::lock_guard<std::mutex> lock(lastUserMtx_);
                            lastUserId_ = ev.userId;
                            lastNickname_ = mioEv->title;
                            lastChannelId_ = mioEv->channelId;
                            lastGroupId_ = ev.groupId;
                        }
                        QueuedOneBotItem item;
                        item.mioEvent = std::move(*mioEv);
                        item.attachments = ev.attachments;
                        std::lock_guard<std::mutex> lock(queueMtx_);
                        queue_.push_back(std::move(item));
                        queueCv_.notify_one();
                    }
                    break;
                }
                case napcat::OneBotPostType::MetaEvent:
                    if (ev.eventType == "lifecycle") {
                        host_.info(kTag, "NapCat 握手完成 self_id=" + ev.selfId);
                    } else {
                        host_.debug(kTag, "meta_event: " + napcat::describeOneBotEvent(ev));
                    }
                    break;
                case napcat::OneBotPostType::Unknown:
                    host_.warn(kTag, "无法识别的事件帧: " + msg->str);
                    break;
                }
            } catch (const std::exception& error) {
                host_.warn(kTag, std::string("帧解析失败: ") + error.what());
            }
            break;
        }
        case ix::WebSocketMessageType::Close: {
            std::lock_guard<std::mutex> lock(connMtx_);
            if (currentWs_ == ws) currentWs_.reset();
            failPending(pending_);
            host_.info(kTag, "NapCat 连接断开");
            break;
        }
        default:
            break;
        }
    }

    void workerLoop() {
        for (;;) {
            std::unique_lock<std::mutex> lock(queueMtx_);
            queueCv_.wait(lock, [this] { return !queue_.empty() || !running_.load(); });
            if (queue_.empty()) {
                return;
            }
            QueuedOneBotItem item = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();

            try {
                if (!item.attachments.empty() && mediaResolver_) {
                    mediaResolver_->resolve(item.attachments, item.mioEvent);
                    host_.info(kTag, "媒体解析完成 count=" +
                                         std::to_string(item.attachments.size()) +
                                         " parts=" +
                                         std::to_string(item.mioEvent.parts.size()));
                }
                host_.postEvent(item.mioEvent.toJson());
            } catch (const std::exception& error) {
                host_.error(kTag, std::string("投递事件失败: ") + error.what());
            }
        }
    }

    sdk::Host& host_;
    NapCatConfig config_;
    std::unique_ptr<MediaResolver> mediaResolver_;

    PendingActions pending_;
    std::mutex connMtx_;
    std::shared_ptr<ix::WebSocket> currentWs_;

    std::deque<QueuedOneBotItem> queue_;
    std::mutex queueMtx_;
    std::condition_variable queueCv_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::unique_ptr<ix::WebSocketServer> server_;

    std::mutex lastUserMtx_;
    std::string lastUserId_;
    std::string lastNickname_;
    std::string lastChannelId_;
    std::string lastGroupId_;
};

} // namespace

} // namespace mio

MIO_PLUGIN(mio::NapCatPlugin, "napcat", "0.1.0")
