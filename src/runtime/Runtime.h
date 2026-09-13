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
//      └─► MemoryStore/MemoryManager（长期记忆：摘要 → BGE-M3 向量 →
//            SQLite BLOB 落库；召回 = SQL 权限预过滤 + SIMD 点积 + 时间
//            衰减 → 由模型通过 recall_memory 工具主动调用）
//
// 工具：get_current_time / remember / set_nickname / set_notes /
//       set_public（模型控制当前上下文公开/私密）/ update_topic（话题上报）/
//       recall_memory（长期记忆召回）/ get_current_user_qq / get_known_person_qq
//
// 严格模块边界：Achieve 只存/读，Router 只路由/融合，Builder 只构建/压缩，
// 记忆系统只写库/召回（归属与注入时机由 Runtime 组装层决定）。
// ============================================================================

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "context/achieve/ColdStart.h"
#include "context/achieve/Achieve.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "context/conversationFusion/FusionRouter.h"
#include "context/inputBuffer/inputBuffer.h"
#include "context/lifecycle/ConversationLifecycle.h"
#include "context/summarizor/SummaryManager.h"
#include "core/contracts/Contracts.h"
#include "core/event/Event.h"
#include "core/eventlog/EventLog.h"
#include "diary/Diary.h"
#include "mind/proposals/CognitionStore.h"
#include "mind/proposals/FactStore.h"
#include "mind/proposals/ProposalStore.h"
#include "providers/embedding/Embedding.h"
#include "providers/llm/Llm.h"
#include "providers/llm/tool/ToolRegistry.h"
#include "providers/memory/MemoryProvider.h"
#include "memory/manager/MemoryManager.h"
#include "mind/facts/Facts.h"
#include "mind/graph/RelationshipGraph.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

struct BotReply {
    ConversationKey conversation;
    std::string text;
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

    // 程序事件审计回执（只更新 lastEventSeq）
    void updateState(const Event& event);
    BotReply ingest(IncomingMessage message);
    RuntimeState state() const;  // 值返回（线程安全快照）
    InputBufferManager& inputBuffers() { return inputBuffers_; }
    const InputBufferManager& inputBuffers() const { return inputBuffers_; }
    std::vector<FusionRouter::UnitInfo> fusionUnitsInfo() const { return router_.unitsInfo(); }
    std::string systemPrompt() const;

    // ---- 管理端只读观测（管理员通道；MUST NOT 注册为模型工具）--------------
    // 待审阅提案列表：pending 提案绝不注入稳定 system prompt，只在此可见。
    std::vector<Proposal> pendingProposals(std::size_t limit) const;
    // 会话生命周期状态（含 summaryPending / retryCount / runState / lastError）
    std::vector<ConversationLifecycleState> lifecycleStates() const;
    // 人工排障复位（清除 Exhausted/Failed，retryCount 归零；不丢弃未提交范围）
    bool clearLifecycleFailure(const std::string& conversationKey);
    // 本次启动以来累计的摘要结果（可观测性）
    struct SummaryStats {
        std::uint64_t succeeded = 0;
        std::uint64_t failed = 0;
        std::uint64_t skippedNoContent = 0;
    };
    SummaryStats summaryStats() const;

private:
    // Facts 渲染（冷启动/重新冷启动后/图谱变化时重建）
    std::string rebuildSystemPrompt(
        const std::vector<DiaryEntry>& cognition = {});
    void registerBuiltinTools();
    void registerMemoryTools();
    // 消息到达的状态快照更新（messageCount/活跃用户/活跃会话）
    void noteMessage(const ConversationKey& conversation,
                     const std::string& senderId);
    // 后台维护线程（单线程 pull 调度 + 摘要执行；不为每条消息建线程）
    void startMaintenance();
    void stopMaintenance();
    void maintenanceLoop();
    // 一轮调度：枚举已有档案会话 → 满足条件才冻结并执行摘要
    void lifecycleTick(std::int64_t now);
    // 执行一个冻结的摘要任务；返回是否成功（失败由调用方回报给调度器）
    bool runSummaryJob(const SummaryJob& job, std::int64_t now);
    // 同步工具调用路径的服务端权限上下文（仅 ingest 线程；后台任务不得读取）
    AccessContext currentAccess() const;
    // 从档案投影收集范围内的参与者 internalId
    std::vector<std::string> collectParticipants(const std::string& convKey,
                                                 std::int64_t fromMessageId,
                                                 std::int64_t toMessageId);
    // 管理端/人工排障：管理员 AccessContext（allowCrossConversation=false 起步）
    AccessContext adminAccess() const;

    void publishConfig(const AppConfig& cfg);  // 热更新落地（服务与计时器）

    // 声明顺序 = 构造顺序；保证成员依赖生命周期正确
    Persona persona_;
    RelationshipGraph graph_;     // 关系图谱（身份/熟悉度/信任度）
    Diary diary_;
    Achieve achieve_;             // 每会话 jsonl 事实源（读给 fusion / 存消息）
    EventLog eventLog_;           // 程序事件审计（append-only JSONL）

    std::shared_ptr<ConfigManager> configMgr_;
    std::shared_ptr<Llm> llm_;
    std::shared_ptr<Embedding> embedding_;  // 向量化（记忆写入/召回 + Router 话题匹配共用）
    std::shared_ptr<SummaryManager> summary_;      // 摘要器（引用 llm_）
    std::shared_ptr<ContextBuilder> builder_;      // 上下文构建 + 重新冷启动
    mutable std::shared_mutex servicesMtx_;        // 保护以上 4 个服务指针的热切换

    MemoryStore memoryStore_;     // SQLite BLOB 向量仓库（data/memory.db）
    MemoryManager memory_;        // 记忆编排：写入/召回（引用 embedding_/memoryStore_）
    FusionRouter router_;         // fusion 即路由（话题语义匹配用 embedding_）
    ToolRegistry registry_;

    // ---- 记忆系统改造新增（身份事实 / 认知 / 提案 / 生命周期）------------
    std::unique_ptr<ProposalStore> proposalStore_;
    std::unique_ptr<FactStore> factStore_;
    std::unique_ptr<CognitionStore> cognitionStore_;
    std::shared_ptr<ConversationLifecycle> lifecycle_;
    // 经历记忆后端（E：本地 SQLite 适配 + 不可用测试桩；Hindsight 为占位）
    std::shared_ptr<MemoryProvider> memoryProvider_;

    std::thread maintenance_;
    std::atomic<bool> stopping_{false};

    std::string systemPrompt_;    // Facts 渲染结果（重建时机由调用方保证）
    RuntimeState state_;
    mutable std::mutex stateMtx_;
    InputBufferManager inputBuffers_;
    std::unique_ptr<AdminServer> adminServer_;

    mutable std::mutex statsMtx_;
    SummaryStats summaryStats_;
};

} // namespace mio
