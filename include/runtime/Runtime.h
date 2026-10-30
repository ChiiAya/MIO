#pragma once

// ============================================================================
// Runtime —— 应用核心（组合根），持续运行，持有全部关键句柄。
//
// 单意识流 MindLoop 架构：
//   adapter/plugin → postEvent(Event)
//      ├─► AttentionGate（注意力裁决 + 全文落盘 Achieve + 投递就绪 Msg）
//      ├─► Achieve（单一时间线全文事件 JSONL 事实源 + 按时间查询）
//      ├─► MindLoop（单意识流防抖 + ContextBuilder 构建/压缩 + ToolLoop）
//      ├─► RelationshipGraph（身份/熟悉度/信任度）
//      └─► 长期记忆由 MCP 提供（如 graphiti_*）
// ============================================================================

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "context/achieve/Achieve.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "context/summarizor/SummaryManager.h"
#include "core/event/AttentionGate.h"
#include "core/event/Event.h"
#include "diary/Diary.h"
#include "providers/embedding/Embedding.h"
#include "providers/llm/Llm.h"
#include "providers/llm/tool/ToolRegistry.h"
#include "mind/facts/Facts.h"
#include "mind/graph/RelationshipGraph.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace mio {

class AdminServer;
namespace mcp {
class McpManager;
}

struct BotReply {
    std::string channelId;
    std::string text;
    bool interrupted = false;
};

struct RuntimeState {
    std::uint64_t version = 0;
    std::uint64_t messageCount = 0;
    std::string botName;
    std::optional<std::string> activeChannel;
    std::set<std::string> activeUsers;
    std::uint64_t lastEventSeq = 0;
};

class Runtime {
public:
    explicit Runtime(std::string botName = "Mio",
                     std::filesystem::path dataDir = "data",
                     std::shared_ptr<Llm> llm = nullptr,
                     std::shared_ptr<ConfigManager> configMgr = nullptr);

    Runtime(std::string botName,
            std::filesystem::path dataDir,
            std::unique_ptr<Llm> llm);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    // 配置热重载
    bool reloadConfig(const std::filesystem::path& configPath = "config.json");
    std::shared_ptr<const AppConfig> config() const;
    std::shared_ptr<ConfigManager> configManager() const;
    std::string systemPrompt() const;

    // 入站事件投递：推入 AttentionGate 并按需唤醒单意识流 MindLoop
    void postEvent(Event event);
    // 等待当前已就绪的事件批次在 MindLoop 中处理完毕（用于 Console 同步交互或测试）
    BotReply flushMindLoop();

    void updateState(const Event& event);
    RuntimeState state() const;

    AttentionGate& attentionGate() { return gate_; }
    const AttentionGate& attentionGate() const { return gate_; }
    Achieve& achieve() { return achieve_; }
    const Achieve& achieve() const { return achieve_; }
    ToolRegistry& toolRegistry() { return registry_; }
    const ToolRegistry& toolRegistry() const { return registry_; }
    RelationshipGraph& graph() { return graph_; }
    const RelationshipGraph& graph() const { return graph_; }
    const std::filesystem::path& dataDir() const { return dataDir_; }

    // 平台消息发送与分段调度
    using OutboundSender = std::function<bool(const Outbound&)>;
    void setOutboundSender(OutboundSender sender);
    bool hasMessageSender() const;
    bool emit(const Outbound& outbound);
    bool emit(const std::string& channelId, const std::string& source, const std::string& rawText);

    // Facts / systemPrompt 的刷新边界（缓存前缀优先）
    void refreshFactsNow(const std::string& reason,
                         std::vector<DiaryEntry> cognition = {});

    // 滚动摘要 → MCP 长期记忆
    void onSummaryToMemory(const SummaryOutcome& outcome);

    // 维护回合：在单意识流上下文副本上跑一次隐藏 LLM 回合
    void runMaintenancePrompt(const std::string& prompt);

private:
    std::string rebuildSystemPrompt(
        const std::vector<DiaryEntry>& cognition = {});
    void registerBuiltinTools();
    void noteEvent(const Event& ev);

    void startMindLoop();
    void stopMindLoop();
    void mindWorkerLoop();
    BotReply processMindTurn();

    std::filesystem::path dataDir_;
    Persona persona_;
    RelationshipGraph graph_;
    Diary diary_;
    Achieve achieve_;
    AttentionGate gate_;

    std::shared_ptr<ConfigManager> configMgr_;
    std::shared_ptr<Llm> llm_;
    std::shared_ptr<Embedding> embedding_;
    std::shared_ptr<SummaryManager> summary_;
    std::shared_ptr<ContextBuilder> builder_;
    mutable std::shared_mutex servicesMtx_;

    ToolRegistry registry_;

    std::string systemPrompt_;
    bool factsDirty_ = false;
    std::vector<DiaryEntry> cognition_;
    RuntimeState state_;
    mutable std::mutex stateMtx_;

    // 单意识流上下文与事件循环状态
    std::vector<Msg> mindContext_;
    mutable std::mutex mindContextMtx_;
    std::mutex mindLoopMtx_;
    std::condition_variable mindWakeCv_;
    std::condition_variable mindIdleCv_;
    std::thread mindThread_;
    bool mindStop_ = false;
    bool mindWakePending_ = false;
    bool mindProcessing_ = false;
    std::atomic<std::uint64_t> mindGeneration_{0};
    int mindDebounceMs_ = 300;
    BotReply lastMindReply_;

    struct LastEventMeta {
        std::string source;
        std::string channelId;
        std::string actorId;
        std::string title;
        std::vector<std::string> replyableIds;
    };
    LastEventMeta lastEventMeta_;
    mutable std::mutex metaMtx_;

    std::unique_ptr<AdminServer> adminServer_;
    OutboundSender outboundSender_;
    mutable std::mutex senderMtx_;
    MessageSplitterConfig splitterConfig_;
    std::unique_ptr<mcp::McpManager> mcpManager_;
    std::unique_ptr<Scheduler> scheduler_;
    std::unique_ptr<SummaryBridge> summaryBridge_;
    MemoryBridgeConfig summaryBridgeConfig_;
    bool bridgeToolMissingLogged_ = false;

    void startScheduler(const MaintenanceConfig& config);
    void startSummaryBridge(const MemoryBridgeConfig& config);
    bool writeSummaryToMemory(const SummaryBridge::Entry& entry);
};

} // namespace mio
