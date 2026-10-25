#pragma once
// ============================================================================
// SummaryBridge —— 把"滚动摘要"喂给 MCP 长期记忆
//
// 背景：内置记忆系统已移除，长期记忆由 MCP 提供（graphiti_add_memory 等）。
// summary 的产出点（冷读取 / 融合重定向 / 重新冷启动）都在 ingest 线程里、
// 且常常持有 unit 锁甚至 Router 的 mapMtx_，所以 sink 里**绝不能**同步做
// 阻塞 I/O；这里退化成"入队 + 后台线程写入"。
//
// 两个关键取舍：
//   * 摘要文本是**累计**的（含旧摘要），所以每个会话只保留最新一条待写入：
//     新的直接替换旧的，天然去重、也避免冷启动时把 N 条近似文本糊上去。
//   * 写入直接调 ToolRegistry 里的 MCP 工具（McpClient 的写管道自带锁、
//     请求按 id 多路复用），不需要再花一次 LLM 调用让模型"转述"。
// ============================================================================

#include <condition_variable>
#include <deque>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/conversation/Conversation.h"

namespace mio {

struct MemoryBridgeConfig {
    bool enabled = true;
    std::string tool = "graphiti_add_memory";  // ToolRegistry 里的 MCP 工具名
    std::string textField = "episode_body";    // 摘要文本写进哪个参数
    // 其余固定参数；字符串值里可用的占位符：{text} {topic} {conv} {private} {group}
    nlohmann::json extraArgs = nlohmann::json::object();
    int maxPending = 32;  // 待写入会话数上限（每会话至多一条）
};

void to_json(nlohmann::json& j, const MemoryBridgeConfig& config);
void from_json(const nlohmann::json& j, MemoryBridgeConfig& config);

// extraArgs 里字符串值支持 {text}/{topic}/{conv}/{group}/{private} 占位符：
// 不必为每种 MCP 记忆服务的参数名改代码。
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
        ConversationKey conversation;
        std::string text;
        std::string topic;
        bool isPublic = false;
        std::string groupId;
    };
    // 返回 false = 本次写入失败（只记日志，不重试、不阻塞）
    using Writer = std::function<bool(const Entry& entry)>;

    SummaryBridge(MemoryBridgeConfig config, Writer writer);
    ~SummaryBridge();
    SummaryBridge(const SummaryBridge&) = delete;
    SummaryBridge& operator=(const SummaryBridge&) = delete;

    void start();
    void stop();  // 可重复调用；析构也会调
    // 入队一条摘要；同一会话保留最新（文本相同则跳过）
    void push(Entry entry);
    std::size_t pending() const;

private:
    void loop();

    // 队列里带单调序号：满了要丢"最早入队"的那条，而不是 map 里 key 最小的那条
    struct Pending {
        Entry entry;
        std::uint64_t seq = 0;
    };

    MemoryBridgeConfig config_;
    Writer writer_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::map<std::string, Pending> pending_;  // 会话 key → 最新待写入
    std::uint64_t nextSeq_ = 0;
    // 已真正写出去的文本（按会话）：累计摘要在两次产出间逐字未变时，
    // 不该把同一段经历重复塞进记忆。用 deque 维护 LRU 上界。
    std::map<std::string, std::string> written_;
    std::deque<std::pair<std::string, std::string>> writtenOrder_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
};

} // namespace mio
