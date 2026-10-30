#include "plugin/OutboundRouter.h"

#include "log/Log.h"
#include "mio/plugin.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <vector>

namespace mio {
namespace {

std::vector<std::string> splitByDelimiter(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> tokens;
    if (delimiter.empty()) {
        std::string s = str;
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        if (!s.empty()) tokens.push_back(std::move(s));
        return tokens;
    }
    std::size_t start = 0;
    std::size_t pos = 0;
    while ((pos = str.find(delimiter, start)) != std::string::npos) {
        std::string token = str.substr(start, pos - start);
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), token.end());
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
        start = pos + delimiter.length();
    }
    if (start < str.length()) {
        std::string token = str.substr(start);
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), token.end());
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

int calculateDelayMs(const std::string& previousText, const MessageSplitterConfig& cfg) {
    if (!cfg.enabled) return 0;
    std::size_t charCount = 0;
    for (std::size_t i = 0; i < previousText.size(); ) {
        unsigned char c = static_cast<unsigned char>(previousText[i]);
        if (c < 0x80) i += 1;
        else if ((c >> 5) == 0x6) i += 2;
        else if ((c >> 4) == 0xE) i += 3;
        else if ((c >> 3) == 0x1E) i += 4;
        else i += 1;
        ++charCount;
    }
    int delay = cfg.baseDelayMs + static_cast<int>(charCount) * cfg.delayPerCharMs;
    if (delay > cfg.maxDelayMs) delay = cfg.maxDelayMs;
    if (delay < 0) delay = 0;
    return delay;
}

} // namespace

bool OutboundRouter::registerChannel(const std::string& owner,
                                     const std::string& source,
                                     const mio_channel_vtbl* vt,
                                     void* user) {
    if (source.empty() || !vt || !vt->send) return false;
    std::lock_guard<std::mutex> lock(mtx_);
    ChannelBinding binding;
    binding.owner = owner;
    binding.source = source;
    binding.vtbl = *vt;
    binding.user = user;
    channels_[source] = std::move(binding);
    log::info("OutboundRouter", "已注册出站通道: source=" + source + " (" + owner + ")");
    return true;
}

void OutboundRouter::unregisterByOwner(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = channels_.begin(); it != channels_.end(); ) {
        if (it->second.owner == owner) {
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

bool OutboundRouter::hasAnyChannel() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return !channels_.empty();
}

bool OutboundRouter::sendBubble(const Outbound& bubble) const {
    ChannelBinding target;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (channels_.empty()) {
            log::warn("OutboundRouter", "无可用出站通道，无法发送: " + bubble.text);
            return false;
        }
        auto it = channels_.find(bubble.source);
        if (it == channels_.end()) {
            it = channels_.begin();
        }
        target = it->second;
    }

    nlohmann::json j = {
        {"source", bubble.source},
        {"channelId", bubble.channelId},
        {"text", bubble.text},
        {"replyTo", bubble.replyTo},
        {"mentions", bubble.mentions}
    };
    if (!bubble.parts.empty()) {
        auto arr = nlohmann::json::array();
        for (const auto& p : bubble.parts) {
            arr.push_back({
                {"kind", partKindToString(p.kind)},
                {"data", p.data},
                {"mimeType", p.mimeType}
            });
        }
        j["parts"] = std::move(arr);
    }

    const std::string dumped = j.dump();
    std::string err;
    const int rc = target.vtbl.send(target.user,
                                    mio_str{dumped.data(), dumped.size()},
                                    &err,
                                    &sdk::detail::stringSinkWrite);
    if (rc != 0 && !err.empty()) {
        log::warn("OutboundRouter", "通道 " + target.source + " 发送失败: " + err);
    }
    return rc == 0;
}

bool OutboundRouter::emit(const Outbound& outbound,
                          const MessageSplitterConfig& splitter) const {
    const auto& rawText = outbound.text;
    if (rawText.empty() && outbound.parts.empty()) return false;

    std::vector<std::string> bubbles;
    if (splitter.enabled && !splitter.delimiter.empty() &&
        rawText.find(splitter.delimiter) != std::string::npos) {
        bubbles = splitByDelimiter(rawText, splitter.delimiter);
    } else {
        std::string s = rawText;
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        if (!s.empty()) {
            bubbles.push_back(std::move(s));
        }
    }

    if (bubbles.empty() && outbound.parts.empty()) return false;
    if (bubbles.empty()) {
        bubbles.push_back("");
    }

    for (std::size_t i = 0; i < bubbles.size(); ++i) {
        if (i > 0 && splitter.enabled) {
            int delayMs = calculateDelayMs(bubbles[i - 1], splitter);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        Outbound bubble;
        bubble.source = outbound.source;
        bubble.channelId = outbound.channelId;
        bubble.text = bubbles[i];
        if (i == 0) {
            bubble.mentions = outbound.mentions;
            bubble.replyTo = outbound.replyTo;
            bubble.parts = outbound.parts;
        }
        if (!sendBubble(bubble)) return false;
    }
    return true;
}

} // namespace mio
