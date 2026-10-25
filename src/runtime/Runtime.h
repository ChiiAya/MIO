#pragma once

// ============================================================================
// Runtime —— 应用核心（组合根），持续运行，持有全部关键句柄。
//
// v7 架构（context · fusion 即路由）：
//   adapter → ingest
//      ├─► FusionRouter.route（身份映射 → 首次遇见 Achieve 冷读取建 unit
//      │     → 融合判断[熟悉/信任/话题/公开] → redirect 融合）
//      ├─► Achieve（每会话 jsonl 事实源：读历史给 fusion / 存消息）
//      ├─► ContextBuilder（信息 → ChatRequest；上下文不够 → 重新冷启动）
//      ├─► RelationshipGraph（身份/熟悉度/信任度）
//      ├─► EventLog（程序事件审计，只写不读）
//      └─► 长期记忆改由 MCP 提供（如 graphiti_*），Runtime 不再内置记忆库
//
// 工具：get_current_time / set_public（模型控制当前上下文公开/私密）/
//       update_topic（话题上报）/ get_current_user_qq /
//       list_contacts / upsert_contact / set_notes
//       （通讯录；set_nickname 已停用，upsert_contact 也不会覆盖已有称呼）/
//       keepsilent / send_message / wait（等待并再次开始思考）
//
// 严格模块边界：Achieve 只存/读，Router 只路由/融合，Builder 只构建/压缩。
// ============================================================================

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "context/achieve/Achieve.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "context/conversationFusion/FusionRouter.h"
#include "context/inputBuffer/inputBuffer.h"
#include "context/summarizor/SummaryManager.h"
#include "core/event/Event.h"
#include "core/eventlog/EventLog.h"
#include "diary/Diary.h"
#include "providers/embedding/Embedding.h"
#include "providers/llm/Llm.h"
#include "providers/llm/tool/ToolRegistry.h"
#include "mind/facts/Facts.h"
#include "mind/graph/RelationshipGraph.h"

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
#include <vector>

namespace mio {

class AdminServer;
namespace mcp {
class McpManager;
}

struct BotReply {
    ConversationKey conversation;
    std::string text;
    bool interrupted = false;
};

struct RuntimeState {
    std::uint64_t version = 0;
    std::uint64_t messageCount = 0;
    std::string botName;
    std::optional<ConversationKey> activeConversation;
    std::set<std::string> activeUsers;
    std::uint64_t lastEventSeq = 0;  // 程序事件日志尾序号（EventLog，仅审计）
};

class Runtime {
public:
    // 构造函数：支持外部注入 Llm / ConfigManager，未传时按配置管理器与环境变量构造
    explicit Runtime(std::string botName = "Mio",
                     std::filesystem::path dataDir = "data",
                     std::shared_ptr<Llm> llm = nullptr,
                     std::shared_ptr<ConfigManager> configMgr = nullptr);

    // 兼容 std::unique_ptr<Llm> 的移动语义重载
    Runtime(std::string botName,
            std::filesystem::path dataDir,
            std::unique_ptr<Llm> llm);
    ~Runtime();

    // 工具闭包捕获成员引用：拷贝/移动都会造成悬空引用，一律禁用
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    // 配置热重载
    bool reloadConfig(const std::filesystem::path& configPath = "config.json");
    std::shared_ptr<const AppConfig> config() const;
    std::shared_ptr<ConfigManager> configManager() const;
    std::string systemPrompt() const;

    // 程序事件审计回执（只更新 lastEventSeq）
    void updateState(const Event& event);
    BotReply ingest(IncomingMessage message);
    RuntimeState state() const;  // 值返回（线程安全快照）
    InputBufferManager& inputBuffers() { return inputBuffers_; }
    const InputBufferManager& inputBuffers() const { return inputBuffers_; }

    // 平台消息发送与分段调度
    using MessageSender = std::function<bool(const ConversationKey& conversation,
                                             const std::string& senderId,
                                             const std::string& text)>;
    using StructuredSender = std::function<bool(const ConversationKey&,
                                                const OutgoingMessage&)>;
    void setMessageSender(MessageSender sender);
    void setStructuredMessageSender(StructuredSender sender);
    bool hasMessageSender() const;
    bool emit(const ConversationKey& conv, const std::string& senderId, const std::string& rawText);
    bool emit(const ConversationKey& conv, const OutgoingMessage& message);

    // Facts / systemPrompt 的刷新边界（缓存前缀优先）。
    // systemPrompt_ 位于 wire 请求的第一位，改一个字节就会让**所有**会话的
    // 整段前缀缓存失效；因此它只在显式边界重建：进程启动、配置重载、
    // 重新冷启动，以及定时任务（daily refresh_facts）。通讯录工具只置脏标记，
    // 由这些边界统一消费。cognition 非空时顺带封存新认知（含日记消费）。
    void refreshFactsNow(const std::string& reason,
                         std::vector<DiaryEntry> cognition = {});

    // 滚动摘要 → MCP 长期记忆：摘要 sink 只入队（不阻塞 ingest 线程），
    // 后台线程直接调用 ToolRegistry 里的 MCP 工具写入。见 SummaryBridge。
    void onSummaryToMemory(const SummaryOutcome& outcome);

    // 维护回合：按 prompt 对"当天有活动的每个会话"各跑一次**隐藏** LLM 回合。
    // 模型可以调用记忆写入等副作用工具（长期记忆由 MCP 提供），但绝不会向平台
    // 发送消息，也不会写进真实会话历史/档案（只在上下文副本上工作）。
    void runMaintenancePrompt(const std::string& prompt);

private:
    // Facts 渲染（冷启动/重新冷启动后/图谱变化时重建）
    std::string rebuildSystemPrompt(
        const std::vector<DiaryEntry>& cognition = {});
    void registerBuiltinTools();
    // 消息到达的状态快照更新（messageCount/活跃用户/活跃会话）
    void noteMessage(const ConversationKey& conversation,
                     const std::string& senderId);

    // 声明顺序 = 构造顺序；保证成员依赖生命周期正确
    Persona persona_;
    RelationshipGraph graph_;     // 关系图谱（身份/熟悉度/信任度）
    Diary diary_;
    Achieve achieve_;             // 每会话 jsonl 事实源（读给 fusion / 存消息）
    EventLog eventLog_;           // 程序事件审计（append-only JSONL）

    std::shared_ptr<ConfigManager> configMgr_;
    std::shared_ptr<Llm> llm_;
    std::shared_ptr<Embedding> embedding_;  // 向量化（记忆写入/召回 + Router 话题匹配共用）
    std::shared_ptr<SummaryManager> summary_;      // 摘要器（引用 llm_；供上下文压缩/融合使用）
    std::shared_ptr<ContextBuilder> builder_;      // 上下文构建 + 重新冷启动
    mutable std::shared_mutex servicesMtx_;        // 保护以上 4 个服务指针的热切换

    FusionRouter router_;         // fusion 即路由（话题语义匹配用 embedding_）
    ToolRegistry registry_;

    std::string systemPrompt_;    // Facts 渲染结果（只在 refreshFactsNow 的边界重建）
    bool factsDirty_ = false;     // 通讯录/认知已变更，等待下一个刷新边界
    std::vector<DiaryEntry> cognition_; // 通讯录更新时重建提示词所需的已封存认知
    RuntimeState state_;
    mutable std::mutex stateMtx_;
    InputBufferManager inputBuffers_;
    std::unique_ptr<AdminServer> adminServer_;
    StructuredSender messageSender_;
    mutable std::mutex senderMtx_;
    MessageSplitterConfig splitterConfig_;
    std::unique_ptr<mcp::McpManager> mcpManager_;
    // 声明在最后 = 最先析构：任务线程必须在其它成员（图谱/日记/服务指针）之前停掉
    std::unique_ptr<Scheduler> scheduler_;
    // 摘要 → MCP 记忆桥：必须在 mcpManager_ 之前停止（它调用 MCP 工具）
    std::unique_ptr<SummaryBridge> summaryBridge_;
    MemoryBridgeConfig summaryBridgeConfig_;
    bool bridgeToolMissingLogged_ = false;

    void startScheduler(const MaintenanceConfig& config);
    void startSummaryBridge(const MemoryBridgeConfig& config);
    bool writeSummaryToMemory(const SummaryBridge::Entry& entry);
};

} // namespace mio
