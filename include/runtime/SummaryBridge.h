#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace mio {

struct MemoryBridgeConfig {
    bool enabled = true;
    std::string tool = "graphiti_add_memory";
    std::string textField = "episode_body";
    nlohmann::json extraArgs = nlohmann::json::object();
    int maxPending = 32;
};

void to_json(nlohmann::json& j, const MemoryBridgeConfig& config);
void from_json(const nlohmann::json& j, MemoryBridgeConfig& config);

inline void substitutePlaceholders(nlohmann::json& value,
                                   const std::map<std::string, std::string>& vars) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        for (const auto& [key, replacement] : vars) {
            const std::string token = "{" + key + "}";
            for (std::size_t pos = text.find(token); pos != std::string::npos;
                 pos = text.find(token, pos + replacement.size())) {
                text.replace(pos, token.size(), replacement);
            }
        }
        value = std::move(text);
        return;
    }
    if (value.is_object()) {
        for (auto& item : value.items()) substitutePlaceholders(item.value(), vars);
        return;
    }
    if (value.is_array()) {
        for (auto& item : value) substitutePlaceholders(item, vars);
    }
}

class SummaryBridge {
public:
    struct Entry {
        std::string channelId;
        std::string text;
        std::string topic;
        bool isPublic = false;
        std::string groupId;
    };
    using Writer = std::function<bool(const Entry& entry)>;

    SummaryBridge(MemoryBridgeConfig config, Writer writer);
    ~SummaryBridge();
    SummaryBridge(const SummaryBridge&) = delete;
    SummaryBridge& operator=(const SummaryBridge&) = delete;

    void start();
    void stop();
    void push(Entry entry);
    std::size_t pending() const;

private:
    void loop();

    struct Pending {
        Entry entry;
        std::uint64_t seq = 0;
    };

    MemoryBridgeConfig config_;
    Writer writer_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::map<std::string, Pending> pending_;
    std::uint64_t nextSeq_ = 0;
    std::map<std::string, std::string> written_;
    std::deque<std::pair<std::string, std::string>> writtenOrder_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
};

} // namespace mio
