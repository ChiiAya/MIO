#pragma once
// ============================================================================
// ConversationLifecycle —— 会话静默摘要调度器（子任务 A，独占目录）
//
// 设计约束（见 docs/operations/memory-system-refactor.md）：
//   * 按【物理 ConversationKey】计时，不按共享 FusionUnit 计时；
//   * 等待条件必须同时包含：静默到期 + 存在未总结消息 + 满足消息阈值 +
//     无待处理输入 + 无生成请求 + 无该会话正在执行的摘要；
//   * 同一会话最多只有一个有效静默任务；旧任务晚到时必须被忽略；
//   * 摘要开始时冻结已落盘范围，成功只能推进到冻结的结束 ID；
//   * 失败保留 summaryPending 与 retryCount，不得当成"已总结"；
//   * 重试耗尽进入可观察失败状态（SUMMARY_RETRY_EXHAUSTED + 人工排障占位），
//     重启不得刷新重试额度；
//   * 工具内部事件不提供入口 —— 不得用它反复延长静默时间；
//   * 不为每条消息创建常驻线程：本类没有后台线程，采用 pull 模型，由 Runtime
//     单线程周期调用 shouldSummarize()/tryBeginSummary()。
//
// 线程安全：所有公开方法内部加互斥锁；Runtime 可以从多个 ingest 线程调用 note*。
//
// 持久化：stateFile 非空时 persist() 原子写（同目录 tmp + rename），带 version
// 字段；load() 失败保持默认且不破坏原文件。落盘时间全部为 epoch seconds
// （system clock），不使用 monotonic 值。
// ============================================================================

#include "core/contracts/LifecycleConfig.h"  // ConversationLifecycleConfig（冻结契约）
#include "core/contracts/SummaryContracts.h" // SummaryJob（冻结契约）
#include "core/conversation/Conversation.h"  // ConversationKey

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mio {

// ---------------------------------------------------------------------------
// 可注入时钟（epoch seconds）—— 调度测试必须能注入假时钟，绝不 sleep 真实秒数
// ---------------------------------------------------------------------------
class IClock {
public:
    virtual ~IClock() = default;
    virtual std::int64_t nowSeconds() const = 0;
};

// 生产实现：system_clock（epoch seconds）。不用 steady_clock，因为 lastActivityAt
// 需要落盘并在重启后继续参与静默判断。
class SystemClock final : public IClock {
public:
    std::int64_t nowSeconds() const override;
};

// ---------------------------------------------------------------------------
// 待总结范围：由 Runtime 用档案投影提供
//
// 语义：
//   * 只统计【已确认落盘】且 id > afterMessageId 的消息；
//   * count 只计用户消息 + 可见助手消息，不计 reasoning / 工具 / 框架提醒；
//   * fromMessageId = 范围内第一条可总结消息 ID（没有任何可总结消息时为 0）；
//   * toMessageId   = 已确认落盘的最大可见消息 ID；
//   * participants 可选：档案投影可顺带给出参与者 internalId；为空时 Runtime
//     必须在投递摘要前补齐（SummaryJob 是值拷贝）。
// 没有任何可总结消息时必须返回 {0, 0, 0}（空范围不得触发摘要）。
// ---------------------------------------------------------------------------
struct PendingRange {
    std::int64_t fromMessageId = 0;  // 未总结范围的起点（0 = 无）
    std::int64_t toMessageId = 0;    // 已确认落盘的最大可见消息 ID
    std::int64_t count = 0;          // 范围内可总结消息数（用户 + 可见助手）
    std::vector<std::string> participants;  // 可选：参与者 internalId
};

using PendingRangeProvider = std::function<PendingRange(const std::string& conversationKey,
                                                        std::int64_t afterMessageId)>;

// 摘要运行状态。Exhausted 是"可观察的失败状态"：不再自动重试，只能人工排障或
// 由管理端调用 clearFailure() 复位。
enum class SummaryRunState { Idle, Running, Failed, Exhausted };

const char* toString(SummaryRunState s);
bool parseSummaryRunState(const std::string& s, SummaryRunState& out);

// ---------------------------------------------------------------------------
// 会话状态快照
// ---------------------------------------------------------------------------
struct ConversationLifecycleState {
    std::string conversationKey;
    std::uint64_t activityVersion = 0;      // 每次有效活动 +1
    std::int64_t lastActivityAt = 0;        // 最后一次有效活动（epoch seconds）
    std::int64_t lastSummarizedMessageId = 0;  // 已提交游标
    std::int64_t pendingFromMessageId = 0;  // 档案投影的待总结范围起点
    std::int64_t pendingCount = 0;          // 档案投影的待总结数量
    int inFlightInputs = 0;                 // 待处理输入（已接收未成批）
    int activeGenerations = 0;              // 进行中的生成请求数
    bool summaryPending = false;            // 是否有未提交的摘要工作（失败后保持 true）
    std::string summaryTaskId;              // 当前唯一有效的静默任务（空 = 无）
    int retryCount = 0;                     // 不含首次尝试
    SummaryRunState runState = SummaryRunState::Idle;
    std::string lastError;
    std::int64_t lastAttemptAt = 0;
    bool hasPendingMessages = false;        // 档案投影事实：当前仍有可总结的未总结消息

    // --- 子任务 A 追加（诊断 + 落盘；不是冻结契约字段）---
    // 最近一次冻结/尝试的范围：成功后保留供审计，失败后随失败状态一起落盘。
    std::int64_t frozenFromMessageId = 0;
    std::int64_t frozenToMessageId = 0;
    std::int64_t frozenCount = 0;
};

// 是否到期 + 稳定原因码（便于 Runtime 记录与测试断言，不做本地化）
//   due / no_state(不会出现，查询会懒建) / summarize_disabled / run_exhausted /
//   summary_in_flight / inputs_pending / generation_active / no_pending_messages /
//   range_start_not_advanced / below_message_threshold / silence_not_elapsed
struct DueDecision {
    bool due = false;
    std::string reason;
};

// ---------------------------------------------------------------------------
// ConversationLifecycle
//
// 典型驱动（pull 模型，Runtime 单线程周期执行）：
//   for (key : 档案投影里的会话列表)
//       if (lc.shouldSummarize(key, now).due)
//           if (auto job = lc.tryBeginSummary(key, now)) 投递给摘要执行者;
//   // 执行者结束（成功/失败/取消）必须二选一回报：
//   lc.completeSummary(job.jobId, job.toMessageId, now);
//   lc.failSummary(job.jobId, err, now);
// ---------------------------------------------------------------------------
class ConversationLifecycle {
public:
    // stateFile 为空 = 不持久化（测试/纯内存模式）。
    // clock 为空时回退到 SystemClock（避免空指针崩溃，并记录警告）。
    ConversationLifecycle(ConversationLifecycleConfig cfg,
                          std::shared_ptr<IClock> clock,
                          std::filesystem::path stateFile = {});
    ~ConversationLifecycle();

    // 没有后台线程，析构不需要 join；但调用方必须保证析构后没有在飞任务回调
    // 进入本对象（建议用 shared_ptr 持有，回收顺序见交付报告）。
    ConversationLifecycle(const ConversationLifecycle&) = delete;
    ConversationLifecycle& operator=(const ConversationLifecycle&) = delete;

    void setPendingRangeProvider(PendingRangeProvider provider);

    // 热更新：非法配置【完整保留旧配置】并记录日志，返回 false（绝不部分套用）。
    // 到期判定不缓存，因此缩短/延长阈值立即对下一次查询生效；关闭静默摘要只
    // 停止新调度，已启动任务仍可完成。
    bool configure(const ConversationLifecycleConfig& cfg);
    ConversationLifecycleConfig config() const;

    // ------------------------------------------------------------------
    // 活动上报（now <= 0 表示使用注入时钟的当前时间）
    //
    // 只有【有效活动】才更新活动版本与活动时间；工具内部事件没有入口。
    // ------------------------------------------------------------------
    // 收到有效消息（含 InputBuffer follower、群聊每个参与者）时调用
    void noteIncomingActivity(const ConversationKey& conv, std::int64_t now = -1);
    // 已接收、等待 debounce 成批：计入待处理输入，等待期间不得起摘要
    void noteInputBuffered(const ConversationKey& conv, std::int64_t now = -1);
    // 成批已被取走送给模型：待处理输入 -1
    void noteInputConsumed(const ConversationKey& conv, std::int64_t now = -1);
    // 生成请求开始：生成期间不得并发读取半成品对话
    void noteGenerationStart(const ConversationKey& conv, std::int64_t now = -1);
    // 生成结束（成功或失败都算）：更新活动时间与版本
    void noteGenerationEnd(const ConversationKey& conv, std::int64_t now = -1);

    // ------------------------------------------------------------------
    // 调度（pull）
    // ------------------------------------------------------------------
    // 纯决策查询：只读状态 + 刷新档案投影缓存，不缓存到期判定。
    // 未知会话会被懒建（lastActivityAt = now，即从"现在"开始计一个静默窗口），
    // 这样启动枚举/重新开启静默摘要都能覆盖未提交范围。
    DueDecision shouldSummarize(const ConversationKey& conv, std::int64_t now = -1) const;

    // 原子地开始一次摘要：置 summaryTaskId、冻结范围、捕获 activityVersion。
    // 不满足等待条件时返回 nullopt（同一会话已有任务时绝不会返回第二个任务）。
    std::optional<SummaryJob> tryBeginSummary(const ConversationKey& conv, std::int64_t now = -1);

    // 摘要成功：只能推进到冻结的结束 ID（toMessageId <= 0 或超过冻结终点时按冻结
    // 终点提交），冻结之后的新消息仍保持待总结；taskId 已不是该会话当前任务时
    // 直接忽略并返回 false（旧任务晚到）。
    bool completeSummary(const std::string& taskId, std::int64_t toMessageId,
                         std::int64_t now = -1);

    // 摘要失败：保留 summaryPending 与冻结范围，retryCount +1（不含首次尝试）；
    // 超过 maxPendingSummaryRetries 时进入 Exhausted，lastError 含
    // SUMMARY_RETRY_EXHAUSTED 与人工排障占位 HUMAN_REVIEW_REQUIRED。
    bool failSummary(const std::string& taskId, const std::string& error,
                     std::int64_t now = -1);

    // 取消（应用关闭/回收在飞任务）：不消耗重试额度，任务转为可重试的 Failed。
    // 调用方必须为每个已开始的 job 调用 complete/fail/cancel 三者之一，否则该会话
    // 会停在 Running 而无法再调度（重启后 load() 会兜底转为 Failed）。
    bool cancelSummary(const std::string& taskId, const std::string& reason,
                       std::int64_t now = -1);

    // 人工排障复位：清除 Exhausted/Failed，retryCount 归零；未提交范围保持待总结。
    // 只应由管理端/人工排障入口调用，MUST NOT 注册为模型工具。
    bool clearFailure(const ConversationKey& conv, std::int64_t now = -1);

    // ------------------------------------------------------------------
    // 观测
    // ------------------------------------------------------------------
    std::vector<std::string> conversations() const;  // 按 key 排序，输出稳定
    // 未知会话返回只填了 conversationKey 的零值状态（不创建状态）
    ConversationLifecycleState snapshot(const std::string& conversationKey) const;
    std::vector<ConversationLifecycleState> snapshots() const;  // 按 key 排序

    // ------------------------------------------------------------------
    // 持久化（原子写 tmp + rename；含 version 字段）
    // ------------------------------------------------------------------
    // 运行中可周期调用（建议间隔 >= 30s）并在每次终态变更后调用；关闭前调用一次。
    bool persist() const;
    // 加载失败（文件不存在/JSON 非法/version 不匹配）保持当前内存状态不变，
    // 绝不改写原文件；成功时整体替换内存状态。
    bool load();

private:
    // 档案投影缓存：键为 (activityVersion, lastSummarizedMessageId)。
    // 只缓存"档案事实"，不缓存到期判定 —— 阈值/开关每次查询都重新判定。
    struct ProjectionCache {
        bool valid = false;
        std::uint64_t activityVersion = 0;
        std::int64_t afterMessageId = -1;
        PendingRange range;
    };

    struct Entry {
        ConversationLifecycleState st;
        ProjectionCache proj;
    };

    // 懒建（queries 也可能建：见 shouldSummarize 注释）。返回引用在
    // unordered_map 里是稳定的（节点式容器）。
    Entry& entryOrCreateLocked(const std::string& key, std::int64_t now) const;
    // 取档案投影（命中缓存则复用）。需要持锁。
    PendingRange projectLocked(Entry& e) const;
    std::int64_t resolveNow(std::int64_t now) const;

    mutable std::mutex mu_;
    // mutable：shouldSummarize 是 const 查询，但要刷新投影缓存与懒建状态。
    mutable std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, std::string> taskIndex_;  // taskId → conversationKey
    PendingRangeProvider provider_;
    ConversationLifecycleConfig cfg_;
    std::shared_ptr<IClock> clock_;
    std::filesystem::path stateFile_;
    std::uint64_t jobSeq_ = 0;
};

} // namespace mio
