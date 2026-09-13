// ============================================================================
// ConversationLifecycle 实现（子任务 A）
//
// 关键设计（为什么这样做）：
//   * pull 模型：没有常驻线程、没有每条消息一个 timer。Runtime 单线程周期调用
//     shouldSummarize/tryBeginSummary，因此不存在"旧线程晚到"的竞态；"旧定时任务
//     晚到"被建模为【已失效的 taskId】—— 只有仍是会话当前任务的结果才被接受。
//   * 到期判定不缓存：每次查询都用当前配置 + 当前状态 + 最新档案投影重算，所以
//     热更新（缩短/延长阈值、关闭开关）立即生效，不需要重建 timers。
//   * 投影缓存以 (activityVersion, lastSummarizedMessageId) 为键：同一 tick 内
//     shouldSummarize + tryBeginSummary 只查一次档案投影；消息到达会 bump 版本，
//     已提交游标推进会改变游标，两者都会让缓存自然失效。
//   * 冻结语义：tryBeginSummary 冻结 [from, to] 与 activityVersion；失败重试时
//     重新投影（范围只增不减），旧任务的结果必须被 taskId 校验挡掉。
// ============================================================================

#include "context/lifecycle/ConversationLifecycle.h"

#include "core/contracts/Errors.h"  // kSummaryRetryExhausted / kHumanReviewRequired
#include "log/Log.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace mio {
namespace {

constexpr int kStateSchemaVersion = 1;
constexpr const char* kTag = "Lifecycle";

std::int64_t systemNowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// 稳定原因码（查询未命中状态等场景）
constexpr const char* kReasonDue = "due";
constexpr const char* kReasonDisabled = "summarize_disabled";
constexpr const char* kReasonExhausted = "run_exhausted";
constexpr const char* kReasonInFlight = "summary_in_flight";
constexpr const char* kReasonInputs = "inputs_pending";
constexpr const char* kReasonGenerating = "generation_active";
constexpr const char* kReasonNoPending = "no_pending_messages";
constexpr const char* kReasonCursor = "range_start_not_advanced";
constexpr const char* kReasonThreshold = "below_message_threshold";
constexpr const char* kReasonSilence = "silence_not_elapsed";

std::string makeJobId(const std::string& convKey, std::int64_t from, std::int64_t to,
                      std::int64_t now, std::uint64_t seq) {
    // 含 epoch 与进程内序号：同一进程内绝不重复，跨重启也几乎不可能撞上
    // （load() 会清空 taskIndex_，撞上也只会命中"当前任务"校验）。
    return "silence-" + std::to_string(now) + "-" + std::to_string(seq) + "-" + convKey +
           "-" + std::to_string(from) + "-" + std::to_string(to);
}

std::string exhaustedMessage(const std::string& error, int retryCount, int maxRetries) {
    // 不伪造审核人与审核时间：这里只放错误码占位，供人工排障入口识别。
    return std::string(kSummaryRetryExhausted) + ": 摘要连续失败 " +
           std::to_string(retryCount) + " 次（maxPendingSummaryRetries=" +
           std::to_string(maxRetries) + "，重试次数不含首次尝试）；需人工排障 " +
           kHumanReviewRequired + "；最后一次错误: " + error;
}

// ---------------------------------------------------------------------------
// 状态序列化：字段缺失/类型错误由 nlohmann 抛出，由调用方按"单条跳过"处理
// ---------------------------------------------------------------------------
nlohmann::json stateToJson(const ConversationLifecycleState& s) {
    return nlohmann::json{
        {"conversationKey", s.conversationKey},
        {"activityVersion", s.activityVersion},
        {"lastActivityAt", s.lastActivityAt},
        {"lastSummarizedMessageId", s.lastSummarizedMessageId},
        {"pendingFromMessageId", s.pendingFromMessageId},
        {"pendingCount", s.pendingCount},
        {"summaryPending", s.summaryPending},
        {"summaryTaskId", s.summaryTaskId},
        {"retryCount", s.retryCount},
        {"runState", toString(s.runState)},
        {"lastError", s.lastError},
        {"lastAttemptAt", s.lastAttemptAt},
        {"hasPendingMessages", s.hasPendingMessages},
        {"frozenFromMessageId", s.frozenFromMessageId},
        {"frozenToMessageId", s.frozenToMessageId},
        {"frozenCount", s.frozenCount},
        // inFlightInputs / activeGenerations 是进程内瞬时计数，不落盘：
        // 重启后没有在途输入与生成，落盘反而会把会话永久卡住。
    };
}

ConversationLifecycleState stateFromJson(const nlohmann::json& j) {
    ConversationLifecycleState s;
    s.conversationKey = j.at("conversationKey").get<std::string>();
    s.activityVersion = j.value("activityVersion", static_cast<std::uint64_t>(0));
    s.lastActivityAt = j.value("lastActivityAt", static_cast<std::int64_t>(0));
    s.lastSummarizedMessageId = j.value("lastSummarizedMessageId", static_cast<std::int64_t>(0));
    s.pendingFromMessageId = j.value("pendingFromMessageId", static_cast<std::int64_t>(0));
    s.pendingCount = j.value("pendingCount", static_cast<std::int64_t>(0));
    s.summaryPending = j.value("summaryPending", false);
    s.summaryTaskId = j.value("summaryTaskId", std::string());
    s.retryCount = j.value("retryCount", 0);
    const std::string runState = j.value("runState", std::string("idle"));
    if (!parseSummaryRunState(runState, s.runState)) {
        s.runState = SummaryRunState::Idle;  // 未知值保守回 Idle，不冒充失败/成功
    }
    s.lastError = j.value("lastError", std::string());
    s.lastAttemptAt = j.value("lastAttemptAt", static_cast<std::int64_t>(0));
    s.hasPendingMessages = j.value("hasPendingMessages", false);
    s.frozenFromMessageId = j.value("frozenFromMessageId", static_cast<std::int64_t>(0));
    s.frozenToMessageId = j.value("frozenToMessageId", static_cast<std::int64_t>(0));
    s.frozenCount = j.value("frozenCount", static_cast<std::int64_t>(0));
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 时钟
// ---------------------------------------------------------------------------
std::int64_t SystemClock::nowSeconds() const { return systemNowSeconds(); }

const char* toString(SummaryRunState s) {
    switch (s) {
    case SummaryRunState::Idle: return "idle";
    case SummaryRunState::Running: return "running";
    case SummaryRunState::Failed: return "failed";
    case SummaryRunState::Exhausted: return "exhausted";
    }
    return "idle";
}

bool parseSummaryRunState(const std::string& s, SummaryRunState& out) {
    if (s == "idle") { out = SummaryRunState::Idle; return true; }
    if (s == "running") { out = SummaryRunState::Running; return true; }
    if (s == "failed") { out = SummaryRunState::Failed; return true; }
    if (s == "exhausted") { out = SummaryRunState::Exhausted; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
ConversationLifecycle::ConversationLifecycle(ConversationLifecycleConfig cfg,
                                             std::shared_ptr<IClock> clock,
                                             std::filesystem::path stateFile)
    : clock_(clock ? std::move(clock) : std::make_shared<SystemClock>()),
      stateFile_(std::move(stateFile)) {
    // 构造期不信任外部配置：非法时退回默认值并记录（调用方通常已用
    // validateConversationLifecycle 校验过）。
    const auto v = validateConversationLifecycle(cfg);
    if (v.ok) {
        cfg_ = cfg;
    } else {
        cfg_ = ConversationLifecycleConfig{};
        log::warn(kTag, std::string("构造时配置非法，回退默认值: ") + v.error);
    }
}

ConversationLifecycle::~ConversationLifecycle() {
    // 没有后台线程/定时器需要回收（pull 模型）。这里只提示"仍有在飞任务"，
    // 真正的回收顺序（停止 tick → complete/fail/cancel → persist → 释放）由
    // Runtime 负责，见交付报告的接线清单。
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : entries_) {
        if (!kv.second.st.summaryTaskId.empty()) {
            log::warn(kTag, "析构时仍有在飞摘要任务未回报（会话 " + kv.first +
                                "，taskId=" + kv.second.st.summaryTaskId +
                                "）；Runtime 必须先回收任务再释放本对象");
        }
    }
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
void ConversationLifecycle::setPendingRangeProvider(PendingRangeProvider provider) {
    std::lock_guard<std::mutex> lk(mu_);
    provider_ = std::move(provider);
    for (auto& kv : entries_) kv.second.proj.valid = false;  // 换了投影源，缓存全部作废
}

bool ConversationLifecycle::configure(const ConversationLifecycleConfig& cfg) {
    const auto v = validateConversationLifecycle(cfg);
    if (!v.ok) {
        // 整次失败：旧配置、旧状态、旧任务全部保持不变，不得部分套用
        log::warn(kTag, std::string("热更新被拒绝（保持旧配置）: ") + v.error);
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
    log::info(kTag, "会话生命周期配置已更新: silence=" +
                        std::to_string(cfg_.silenceTimeoutSeconds) + "s, onSilence=" +
                        (cfg_.summarizeOnSilence ? "true" : "false") + ", minMessages=" +
                        std::to_string(cfg_.minMessagesBeforeSummary) + ", maxRetries=" +
                        std::to_string(cfg_.maxPendingSummaryRetries));
    return true;
}

ConversationLifecycleConfig ConversationLifecycle::config() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

// ---------------------------------------------------------------------------
// 内部：状态查找 / 投影
// ---------------------------------------------------------------------------
std::int64_t ConversationLifecycle::resolveNow(std::int64_t now) const {
    if (now > 0) return now;
    return clock_ ? clock_->nowSeconds() : 0;
}

ConversationLifecycle::Entry& ConversationLifecycle::entryOrCreateLocked(
    const std::string& key, std::int64_t now) const {
    auto it = entries_.find(key);
    if (it != entries_.end()) return it->second;
    Entry e;
    e.st.conversationKey = key;
    // 懒建会话从"现在"起算一个静默窗口：既不漏掉重启/重新开启前的未提交范围，
    // 又不会在启动瞬间对全部历史会话同时打摘要请求。
    e.st.lastActivityAt = now;
    return entries_.emplace(key, std::move(e)).first->second;
}

PendingRange ConversationLifecycle::projectLocked(Entry& e) const {
    const ConversationLifecycleState& st = e.st;
    if (e.proj.valid && e.proj.activityVersion == st.activityVersion &&
        e.proj.afterMessageId == st.lastSummarizedMessageId) {
        return e.proj.range;
    }
    PendingRange r;
    if (provider_) {
        r = provider_(st.conversationKey, st.lastSummarizedMessageId);
    }
    // 规范化：空范围、倒挂范围一律视为"没有可总结消息"，绝不触发摘要。
    if (r.count <= 0 || r.fromMessageId <= 0 || r.toMessageId < r.fromMessageId) {
        r.fromMessageId = 0;
        r.toMessageId = 0;
        r.count = 0;
    } else if (r.fromMessageId <= st.lastSummarizedMessageId) {
        // 投影实现错误（没有遵守 afterMessageId）：宁可不总结，也不重复总结已提交范围
        log::warn(kTag, "档案投影返回了已提交范围之前的数据（会话 " + st.conversationKey +
                            "，from=" + std::to_string(r.fromMessageId) + " <= cursor=" +
                            std::to_string(st.lastSummarizedMessageId) + "），本次不调度");
        r.fromMessageId = 0;
        r.toMessageId = 0;
        r.count = 0;
    }
    e.proj.valid = true;
    e.proj.activityVersion = st.activityVersion;
    e.proj.afterMessageId = st.lastSummarizedMessageId;
    e.proj.range = r;
    return r;
}

// ---------------------------------------------------------------------------
// 活动上报
// ---------------------------------------------------------------------------
void ConversationLifecycle::noteIncomingActivity(const ConversationKey& conv,
                                                 std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    const std::string key = conv.toString();
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = entryOrCreateLocked(key, t).st;
    ++st.activityVersion;  // 有效活动 → 版本 +1（使旧计划的判定失效）
    st.lastActivityAt = std::max(st.lastActivityAt, t);
}

void ConversationLifecycle::noteInputBuffered(const ConversationKey& conv, std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = entryOrCreateLocked(conv.toString(), t).st;
    ++st.inFlightInputs;  // 已接收待成批：等待期间不得起摘要
}

void ConversationLifecycle::noteInputConsumed(const ConversationKey& conv, std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = entryOrCreateLocked(conv.toString(), t).st;
    if (st.inFlightInputs > 0) --st.inFlightInputs;  // 防御重复上报，不得变负
}

void ConversationLifecycle::noteGenerationStart(const ConversationKey& conv,
                                                std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = entryOrCreateLocked(conv.toString(), t).st;
    // 生成请求本身不是"新活动"（发起它的消息已经上报过），只挡住并发摘要
    ++st.activeGenerations;
}

void ConversationLifecycle::noteGenerationEnd(const ConversationKey& conv, std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    const std::string key = conv.toString();
    std::lock_guard<std::mutex> lk(mu_);
    auto& st = entryOrCreateLocked(key, t).st;
    if (st.activeGenerations > 0) --st.activeGenerations;
    // 模型输出完成也算有效活动：更新版本与时间（同一轮对话仍在继续）
    ++st.activityVersion;
    st.lastActivityAt = std::max(st.lastActivityAt, t);
}

// ---------------------------------------------------------------------------
// 调度
// ---------------------------------------------------------------------------
DueDecision ConversationLifecycle::shouldSummarize(const ConversationKey& conv,
                                                   std::int64_t now) const {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    // 懒建：查询即注册（见头文件说明）。投影随之刷新，但不写任何"到期"结论。
    Entry& e = entryOrCreateLocked(conv.toString(), t);
    const ConversationLifecycleState& st = e.st;
    // 先刷新投影（每 (activityVersion, 游标) 至多查一次）：即使当前关闭了静默摘要，
    // 管理端也能看到真实积压量，便于判断能否安全打开开关。
    const PendingRange r = projectLocked(e);

    DueDecision d;
    if (!cfg_.summarizeOnSilence) {
        d.reason = kReasonDisabled;
        return d;
    }
    if (st.runState == SummaryRunState::Exhausted) {
        d.reason = kReasonExhausted;
        return d;
    }
    if (!st.summaryTaskId.empty()) {
        d.reason = kReasonInFlight;  // 同一会话最多一个有效任务
        return d;
    }
    if (st.inFlightInputs > 0) {
        d.reason = kReasonInputs;
        return d;
    }
    if (st.activeGenerations > 0) {
        d.reason = kReasonGenerating;  // 模型仍在生成：延迟摘要，不读半成品
        return d;
    }
    if (r.count <= 0 || r.fromMessageId <= 0) {
        d.reason = kReasonNoPending;  // 空范围不得总结
        return d;
    }
    if (r.fromMessageId <= st.lastSummarizedMessageId) {
        d.reason = kReasonCursor;
        return d;
    }
    if (cfg_.minMessagesBeforeSummary > 0 &&
        r.count < static_cast<std::int64_t>(cfg_.minMessagesBeforeSummary)) {
        d.reason = kReasonThreshold;
        return d;
    }
    if (st.lastActivityAt + cfg_.silenceTimeoutSeconds > t) {
        d.reason = kReasonSilence;
        return d;
    }
    d.due = true;
    d.reason = kReasonDue;
    return d;
}

std::optional<SummaryJob> ConversationLifecycle::tryBeginSummary(const ConversationKey& conv,
                                                                 std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    Entry& e = entryOrCreateLocked(conv.toString(), t);
    ConversationLifecycleState& st = e.st;

    // 与 shouldSummarize 相同的等待条件，这里再判一次：从"问"到"起"之间状态可能
    // 已经变化（多线程 ingest），冻结必须原子完成。
    if (!cfg_.summarizeOnSilence) return std::nullopt;
    if (st.runState == SummaryRunState::Exhausted) return std::nullopt;
    if (!st.summaryTaskId.empty()) return std::nullopt;
    if (st.inFlightInputs > 0) return std::nullopt;
    if (st.activeGenerations > 0) return std::nullopt;

    const PendingRange r = projectLocked(e);
    if (r.count <= 0 || r.fromMessageId <= 0) return std::nullopt;
    if (r.fromMessageId <= st.lastSummarizedMessageId) return std::nullopt;
    if (cfg_.minMessagesBeforeSummary > 0 &&
        r.count < static_cast<std::int64_t>(cfg_.minMessagesBeforeSummary)) {
        return std::nullopt;
    }
    if (st.lastActivityAt + cfg_.silenceTimeoutSeconds > t) return std::nullopt;

    SummaryJob job;
    job.jobId = makeJobId(st.conversationKey, r.fromMessageId, r.toMessageId, t, ++jobSeq_);
    job.conversationKey = st.conversationKey;
    job.participants = r.participants;
    job.kind = SummaryKind::EpisodicMemory;  // 静默摘要 = 经历记忆
    job.fromMessageId = r.fromMessageId;     // 冻结起点
    job.toMessageId = r.toMessageId;         // 冻结终点（成功后只能推进到这里）
    job.frozenAt = t;                        // epoch seconds（落盘时间，禁 monotonic）
    job.activityVersion = st.activityVersion;// 冻结活动版本（旧任务校验用）
    job.attempt = st.retryCount;             // 不含首次尝试
    job.visibility = Visibility::Conversation;  // 默认最窄；摘要只能建议收窄
    job.source = "silence_summary";

    st.summaryTaskId = job.jobId;
    st.runState = SummaryRunState::Running;
    st.lastAttemptAt = t;
    st.summaryPending = true;
    st.hasPendingMessages = true;
    st.pendingFromMessageId = r.fromMessageId;
    st.pendingCount = r.count;
    st.frozenFromMessageId = r.fromMessageId;
    st.frozenToMessageId = r.toMessageId;
    st.frozenCount = r.count;
    taskIndex_[job.jobId] = st.conversationKey;

    if (job.participants.empty()) {
        // 后台工作必须显式携带参与者：这里不伪造，只提醒 Runtime 在投递前补齐
        log::warn(kTag, "静默任务参与者为空（会话 " + job.conversationKey +
                            "），Runtime 必须在投递摘要前用档案投影补齐");
    }
    return job;
}

bool ConversationLifecycle::completeSummary(const std::string& taskId,
                                            std::int64_t toMessageId, std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    const auto idx = taskIndex_.find(taskId);
    if (idx == taskIndex_.end()) {
        log::warn(kTag, "忽略未知/已完成任务的完成回报: " + taskId);
        return false;
    }
    const auto it = entries_.find(idx->second);
    if (it == entries_.end()) {
        taskIndex_.erase(idx);
        return false;
    }
    ConversationLifecycleState& st = it->second.st;
    if (st.summaryTaskId != taskId) {
        // 旧任务晚到：它早已被新的任务取代（例如失败后重试），不得提交、不得清状态
        log::warn(kTag, "忽略过期摘要任务的完成回报（会话 " + st.conversationKey +
                            "，过期 taskId=" + taskId + "，当前 taskId=" + st.summaryTaskId + "）");
        return false;
    }

    const std::int64_t frozenTo = st.frozenToMessageId;
    std::int64_t commitTo = toMessageId;
    if (commitTo <= 0) commitTo = frozenTo;  // 未提供时按冻结终点提交
    if (commitTo > frozenTo) {
        log::warn(kTag, "摘要提交超过冻结结束 ID，已截断（会话 " + st.conversationKey +
                            "，请求 to=" + std::to_string(toMessageId) + "，冻结 to=" +
                            std::to_string(frozenTo) + "）");
        commitTo = frozenTo;
    }
    if (commitTo < st.frozenFromMessageId) {
        log::warn(kTag, "摘要提交范围倒挂，忽略（会话 " + st.conversationKey + "）");
        return false;
    }
    if (commitTo > st.lastSummarizedMessageId) {
        st.lastSummarizedMessageId = commitTo;  // 游标只前进
    }

    taskIndex_.erase(idx);
    st.summaryTaskId.clear();
    st.runState = SummaryRunState::Idle;
    st.retryCount = 0;  // 只有成功才归还重试额度
    st.lastError.clear();

    // 冻结之后到达的新消息必须继续待总结：重新投影（游标已变 → 缓存自然失效）
    const PendingRange r = projectLocked(it->second);
    st.pendingFromMessageId = r.fromMessageId;
    st.pendingCount = r.count;
    st.hasPendingMessages = (r.count > 0);
    st.summaryPending = st.hasPendingMessages;
    log::debug(kTag, "会话 " + st.conversationKey + " 摘要已提交到消息 " +
                        std::to_string(st.lastSummarizedMessageId) + "（本次尝试耗时 " +
                        std::to_string(t - st.lastAttemptAt) + "s，剩余待总结 " +
                        std::to_string(st.pendingCount) + " 条）");
    return true;
}

bool ConversationLifecycle::failSummary(const std::string& taskId, const std::string& error,
                                        std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    const auto idx = taskIndex_.find(taskId);
    if (idx == taskIndex_.end()) {
        log::warn(kTag, "忽略未知/已完成任务的失败回报: " + taskId);
        return false;
    }
    const auto it = entries_.find(idx->second);
    if (it == entries_.end()) {
        taskIndex_.erase(idx);
        return false;
    }
    ConversationLifecycleState& st = it->second.st;
    if (st.summaryTaskId != taskId) {
        log::warn(kTag, "忽略过期摘要任务的失败回报（会话 " + st.conversationKey +
                            "，过期 taskId=" + taskId + "）");
        return false;
    }

    taskIndex_.erase(idx);
    st.summaryTaskId.clear();
    st.lastAttemptAt = t;
    if (st.retryCount < 1000000) ++st.retryCount;  // 不含首次尝试

    if (st.retryCount > cfg_.maxPendingSummaryRetries) {
        st.runState = SummaryRunState::Exhausted;
        st.lastError = exhaustedMessage(error, st.retryCount, cfg_.maxPendingSummaryRetries);
        log::error(kTag, "会话 " + st.conversationKey + " 摘要重试耗尽: " + st.lastError);
    } else {
        st.runState = SummaryRunState::Failed;
        st.lastError = error;
        log::warn(kTag, "会话 " + st.conversationKey + " 摘要失败（第 " +
                            std::to_string(st.retryCount) + " 次重试机会 / 上限 " +
                            std::to_string(cfg_.maxPendingSummaryRetries) + "）: " + error);
    }

    // 失败不得被当成"已总结"：范围仍未提交，summaryPending 必须保持 true。
    st.summaryPending = true;
    const PendingRange r = projectLocked(it->second);
    st.pendingFromMessageId = r.fromMessageId > 0 ? r.fromMessageId : st.frozenFromMessageId;
    st.pendingCount = r.count > 0 ? r.count : st.frozenCount;
    st.hasPendingMessages = (r.count > 0);
    return true;
}

bool ConversationLifecycle::cancelSummary(const std::string& taskId, const std::string& reason,
                                          std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    const auto idx = taskIndex_.find(taskId);
    if (idx == taskIndex_.end()) return false;
    const auto it = entries_.find(idx->second);
    if (it == entries_.end()) {
        taskIndex_.erase(idx);
        return false;
    }
    ConversationLifecycleState& st = it->second.st;
    if (st.summaryTaskId != taskId) return false;

    taskIndex_.erase(idx);
    st.summaryTaskId.clear();
    st.lastAttemptAt = t;
    // 取消（关闭/回收）不是模型失败：不消耗重试额度
    if (st.runState != SummaryRunState::Exhausted) st.runState = SummaryRunState::Failed;
    st.lastError = "摘要任务已取消（关闭/回收）: " + reason;
    st.summaryPending = true;  // 范围仍未提交
    const PendingRange r = projectLocked(it->second);
    st.pendingFromMessageId = r.fromMessageId > 0 ? r.fromMessageId : st.frozenFromMessageId;
    st.pendingCount = r.count > 0 ? r.count : st.frozenCount;
    st.hasPendingMessages = (r.count > 0);
    return true;
}

bool ConversationLifecycle::clearFailure(const ConversationKey& conv, std::int64_t now) {
    const std::int64_t t = resolveNow(now);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(conv.toString());
    if (it == entries_.end()) return false;
    ConversationLifecycleState& st = it->second.st;
    if (st.runState != SummaryRunState::Exhausted && st.runState != SummaryRunState::Failed) {
        return false;
    }
    st.runState = SummaryRunState::Idle;
    st.retryCount = 0;   // 人工复位才归还额度（重启不会归还）
    st.lastError.clear();
    st.lastAttemptAt = t;
    log::info(kTag, "人工排障复位会话 " + st.conversationKey + " 的摘要失败状态");
    return true;
}

// ---------------------------------------------------------------------------
// 观测
// ---------------------------------------------------------------------------
std::vector<std::string> ConversationLifecycle::conversations() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> keys;
    keys.reserve(entries_.size());
    for (const auto& kv : entries_) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

ConversationLifecycleState ConversationLifecycle::snapshot(
    const std::string& conversationKey) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = entries_.find(conversationKey);
    if (it == entries_.end()) {
        ConversationLifecycleState s;
        s.conversationKey = conversationKey;  // 未知会话：零值 + 回填 key，不创建状态
        return s;
    }
    return it->second.st;
}

std::vector<ConversationLifecycleState> ConversationLifecycle::snapshots() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ConversationLifecycleState> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.second.st);
    std::sort(out.begin(), out.end(),
              [](const ConversationLifecycleState& a, const ConversationLifecycleState& b) {
                  return a.conversationKey < b.conversationKey;
              });
    return out;
}

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------
bool ConversationLifecycle::persist() const {
    if (stateFile_.empty()) {
        log::debug(kTag, "未配置状态文件，跳过持久化");
        return false;
    }

    // 先在锁内做快照，锁外写文件：避免阻塞 ingest 线程的 note* 调用
    nlohmann::json doc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        doc["version"] = kStateSchemaVersion;
        doc["savedAt"] = clock_ ? clock_->nowSeconds() : 0;
        auto arr = nlohmann::json::array();
        for (const auto& kv : entries_) arr.push_back(stateToJson(kv.second.st));
        doc["conversations"] = std::move(arr);
    }

    std::error_code ec;
    const auto parent = stateFile_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            log::error(kTag, "创建状态目录失败: " + parent.string() + " (" + ec.message() + ")");
            return false;
        }
    }

    // 原子写：同目录 tmp + rename（同文件系统内 rename 为原子替换）。
    // 中途失败时删除 tmp，原文件保持不动。
    std::filesystem::path tmp = stateFile_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            log::error(kTag, "无法写入临时状态文件: " + tmp.string());
            return false;
        }
        out << doc.dump(2) << "\n";
        out.flush();
        if (!out) {
            log::error(kTag, "写入临时状态文件失败: " + tmp.string());
            out.close();
            std::error_code rmEc;
            std::filesystem::remove(tmp, rmEc);
            return false;
        }
    }
    std::filesystem::rename(tmp, stateFile_, ec);
    if (ec) {
        log::error(kTag, "rename 状态文件失败: " + stateFile_.string() + " (" + ec.message() + ")");
        std::error_code rmEc;
        std::filesystem::remove(tmp, rmEc);
        return false;
    }
    return true;
}

bool ConversationLifecycle::load() {
    if (stateFile_.empty()) return false;
    std::ifstream in(stateFile_, std::ios::binary);
    if (!in) {
        log::info(kTag, "状态文件不存在，保持默认: " + stateFile_.string());
        return false;
    }

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& e) {
        // 加载失败：不破坏原文件，也不改动内存状态
        log::error(kTag, std::string("状态文件解析失败（保持默认且不改写原文件）: ") + e.what());
        return false;
    }
    if (!doc.is_object() || !doc.contains("version") || !doc["version"].is_number_integer() ||
        doc["version"].get<int>() != kStateSchemaVersion) {
        log::error(kTag, "状态文件 version 缺失或不匹配，拒绝加载: " + stateFile_.string());
        return false;
    }

    std::unordered_map<std::string, Entry> loaded;
    std::size_t skipped = 0;
    if (doc.contains("conversations") && doc["conversations"].is_array()) {
        for (const auto& item : doc["conversations"]) {
            try {
                if (!item.is_object()) { ++skipped; continue; }
                ConversationLifecycleState st = stateFromJson(item);
                if (st.conversationKey.empty()) { ++skipped; continue; }
                Entry e;
                e.st = std::move(st);
                loaded.emplace(e.st.conversationKey, std::move(e));
            } catch (const std::exception&) {
                ++skipped;  // 单条损坏：跳过该条，不影响其他会话
            }
        }
    }

    // 崩溃恢复：新进程里没有在飞任务的执行者，把 Running 视为"被中断"，
    // 转为可重试的 Failed；retryCount 原样保留（重启不得刷新重试额度）。
    for (auto& kv : loaded) {
        ConversationLifecycleState& st = kv.second.st;
        if (st.runState == SummaryRunState::Running || !st.summaryTaskId.empty()) {
            const std::string interruptedTask = st.summaryTaskId;
            st.summaryTaskId.clear();
            if (st.runState == SummaryRunState::Running) {
                st.runState = SummaryRunState::Failed;
                st.lastError = "摘要任务在上次进程退出前未完成（taskId=" + interruptedTask +
                               "），已转为待重试";
            }
            log::warn(kTag, "崩溃恢复：会话 " + st.conversationKey + " 的在飞任务已作废（" +
                                interruptedTask + "）");
        }
    }

    std::size_t restored = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_ = std::move(loaded);
        taskIndex_.clear();
        jobSeq_ = 0;
        restored = entries_.size();
    }
    log::info(kTag, "状态文件已加载: " + stateFile_.string() + "（会话 " +
                        std::to_string(restored) + "，跳过 " + std::to_string(skipped) + " 条）");
    return true;
}

} // namespace mio
