#pragma once
// ============================================================================
// Runtime —— 应用核心组合根（Composition Root）
//
// 职责：
//   1. 组装并持有核心基础设施：Achieve、AttentionGate、RelationshipGraph、
//      ToolRegistry、McpManager、SummaryBridge、Scheduler、AdminServer、MindLoop；
//   2. 接收插件投递的入站事件（postEvent）：过 Hook → 更新人物图谱 → 入注意力门 → 唤醒 MindLoop；
//   3. 转发平台出站请求（emit），在维护回合自动拦截外发消息；
//   4. 统一处理启动装配与在线配置热重载（reloadConfig）。
// ============================================================================

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "context/achieve/Achieve.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "core/event/AttentionGate.h"
#include "core/event/Event.h"
#include "mind/MindLoop.h"
#include "mind/facts/Facts.h"
#include "mind/graph/RelationshipGraph.h"
#include "providers/llm/Llm.h"
#include "providers/llm/tool/ToolRegistry.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

namespace mio {

class AdminServer;
class PluginManager;
namespace mcp {
class McpManager;
}

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
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    // 配置查询与在线热重载
    bool reloadConfig(const std::filesystem::path& configPath = "config.json");
    std::shared_ptr<const AppConfig> config() const;
    std::shared_ptr<ConfigManager> configManager() const { return configMgr_; }
    std::string systemPrompt() const;

    // 入站事件投递
    void postEvent(Event event);
    RuntimeState state() const;

    // 核心子模块访问器
    AttentionGate& attentionGate() { return gate_; }
    const AttentionGate& attentionGate() const { return gate_; }
    Achieve& achieve() { return achieve_; }
    const Achieve& achieve() const { return achieve_; }
    ToolRegistry& toolRegistry() { return registry_; }
    const ToolRegistry& toolRegistry() const { return registry_; }
    RelationshipGraph& graph() { return graph_; }
    const RelationshipGraph& graph() const { return graph_; }
    const std::filesystem::path& dataDir() const { return dataDir_; }

    // 插件桥接回调注入（由 PluginManager 挂载）
    using OutboundSender = std::function<bool(const Outbound&)>;
    using EventFilter = std::function<bool(Event&)>;
    using BeforeLlmHook = std::function<void(std::vector<Msg>&)>;

    void setOutboundSender(OutboundSender sender);
    void setEventFilter(EventFilter filter);
    void setBeforeLlmHook(BeforeLlmHook hook);
    void setPluginManager(PluginManager* pm);
    PluginManager* pluginManager() const;

    // 出站发送入口
    bool hasMessageSender() const;
    bool emit(const Outbound& outbound);
    bool emit(const std::string& channelId, const std::string& source, const std::string& rawText);

    // 重建稳定 System Prompt（保护缓存前缀）
    void refreshFactsNow(const std::string& reason);

private:
    void registerBuiltinTools();
    void applyConfig(const AppConfig& cfg, std::shared_ptr<Llm> customLlm, const std::string& reason);

    std::filesystem::path dataDir_;
    std::shared_ptr<ConfigManager> configMgr_;

    RelationshipGraph graph_;
    Achieve achieve_;
    AttentionGate gate_;
    ToolRegistry registry_;

    Persona persona_;
    std::shared_ptr<Llm> llm_;
    std::shared_ptr<ContextBuilder> builder_;
    std::string systemPrompt_;
    bool factsDirty_ = false;
    mutable std::shared_mutex servicesMtx_;

    RuntimeState state_;
    mutable std::mutex stateMtx_;

    OutboundSender outboundSender_;
    EventFilter eventFilter_;
    BeforeLlmHook beforeLlmHook_;
    PluginManager* pluginManager_ = nullptr;
    mutable std::mutex hooksMtx_;

    std::unique_ptr<MindLoop> mindLoop_;
    std::unique_ptr<mcp::McpManager> mcpManager_;
    std::unique_ptr<AdminServer> adminServer_;
};

} // namespace mio
