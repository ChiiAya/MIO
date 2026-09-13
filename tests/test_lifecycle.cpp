// ============================================================================
// 子任务 A：会话静默摘要调度器用例（tests/test_lifecycle.cpp）
//
// 覆盖文档「必须覆盖的验收用例」中属于 A 的部分：
//   T01 会话独立计时 / 后台结果归属
//   T02 follower 到达、生成中、旧定时任务晚到
//   T03 总结中收到新消息 → 冻结范围提交、新消息仍待总结
//   T04 非法热更新、缩短/延长阈值、关闭/重新开启
//   重试耗尽（SUMMARY_RETRY_EXHAUSTED + 人工排障占位）
//   持久化（retryCount / 冻结范围跨实例保留；加载失败不破坏原文件）
//   消息阈值边界；可注入时钟（全部用假时钟推进，绝不 sleep 真实秒数）
// ============================================================================

#include "framework.h"

#include "context/lifecycle/ConversationLifecycle.h"
#include "core/contracts/Errors.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using namespace mio;

namespace {

// ---------------------------------------------------------------------------
// 测试替身：假时钟 + 假档案投影（不使用真实用户数据、不联网）
// ---------------------------------------------------------------------------
struct FakeClock final : public IClock {
    std::int64_t t = 1700000000;  // 远离 0 的 epoch 起点，便于发现"用了真实时钟"
    std::int64_t nowSeconds() const override { return t; }
    void advance(std::int64_t seconds) { t += seconds; }
};

// 档案投影 fake：每个物理会话一份升序的"可总结消息 ID"列表
struct FakeArchive {
    std::map<std::string, std::vector<std::int64_t>> messages;
    std::vector<std::string> participants{"user-1"};
    bool withParticipants = true;

    void add(const std::string& key, std::int64_t id) { messages[key].push_back(id); }

    PendingRangeProvider provider() {
        return [this](const std::string& key, std::int64_t after) {
            PendingRange r;
            const auto it = messages.find(key);
            if (it == messages.end()) return r;
            for (std::int64_t id : it->second) {
                if (id <= after) continue;  // 只统计游标之后、已确认落盘的消息
                if (r.fromMessageId == 0) r.fromMessageId = id;
                r.toMessageId = id;
                ++r.count;
            }
            if (withParticipants) r.participants = participants;
            return r;
        };
    }
};

ConversationLifecycleConfig cfgOf(int silenceSeconds, bool onSilence, int minMessages,
                                  int maxRetries) {
    ConversationLifecycleConfig c;
    c.silenceTimeoutSeconds = silenceSeconds;
    c.summarizeOnSilence = onSilence;
    c.minMessagesBeforeSummary = minMessages;
    c.maxPendingSummaryRetries = maxRetries;
    return c;
}

// 每个用例独立的临时目录（不碰真实数据目录）
std::filesystem::path tempDir(const std::string& tag) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path() /
               ("mio_lifecycle_test_" + tag + "_" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

const ConversationKey kA = ConversationKey::privateChat("A");
const ConversationKey kB = ConversationKey::privateChat("B");
const ConversationKey kC = ConversationKey::privateChat("C");

} // namespace

// ---------------------------------------------------------------------------
// 时钟
// ---------------------------------------------------------------------------
MIO_TEST(A_可注入时钟_推进假时钟立即生效) {
    // 生产时钟是 epoch seconds（不是 monotonic），可落盘、可跨重启比较
    CHECK_TRUE(SystemClock().nowSeconds() > 1600000000);

    auto clock = std::make_shared<FakeClock>();
    clock->t = 1700000000;
    FakeArchive archive;
    archive.add(kA.toString(), 1);

    ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());

    lc.noteIncomingActivity(kA, clock->t);
    CHECK_EQ(lc.snapshot(kA.toString()).lastActivityAt, static_cast<std::int64_t>(1700000000));

    clock->advance(9);
    CHECK_FALSE(lc.shouldSummarize(kA, clock->t).due);
    clock->advance(1);  // 静默正好到期
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
    // 不传 now 时使用注入时钟（now <= 0 语义）
    CHECK_TRUE(lc.shouldSummarize(kA, -1).due);
}

// ---------------------------------------------------------------------------
// T01：会话独立计时 + 后台结果归属
// ---------------------------------------------------------------------------
MIO_TEST(T01_会话独立计时_只总结到期会话) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);
    archive.add(kB.toString(), 1);
    archive.add(kB.toString(), 2);

    ConversationLifecycle lc(cfgOf(60, true, 2, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());

    lc.noteIncomingActivity(kA, clock->t);
    lc.noteIncomingActivity(kB, clock->t);

    clock->advance(59);
    CHECK_FALSE(lc.shouldSummarize(kA, clock->t).due);
    CHECK_FALSE(lc.shouldSummarize(kB, clock->t).due);

    // B 又来了新消息：只有 A 的静默窗口走完
    lc.noteIncomingActivity(kB, clock->t);
    clock->advance(1);

    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
    const DueDecision dB = lc.shouldSummarize(kB, clock->t);
    CHECK_FALSE(dB.due);
    CHECK_EQ(dB.reason, std::string("silence_not_elapsed"));

    auto jobA = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(jobA.has_value());
    // 后台结果归属：任务必须带物理会话键与冻结范围
    CHECK_EQ(jobA->conversationKey, kA.toString());
    CHECK_EQ(jobA->fromMessageId, static_cast<std::int64_t>(1));
    CHECK_EQ(jobA->toMessageId, static_cast<std::int64_t>(2));
    CHECK_EQ(jobA->source, std::string("silence_summary"));
    CHECK_TRUE(jobA->kind == SummaryKind::EpisodicMemory);
    CHECK_TRUE(jobA->valid());
    CHECK_TRUE(jobA->visibility == Visibility::Conversation);
    CHECK_EQ(jobA->participants.size(), static_cast<std::size_t>(1));

    // B 未到期 → 不得起任务
    CHECK_FALSE(lc.tryBeginSummary(kB, clock->t).has_value());

    CHECK_TRUE(lc.completeSummary(jobA->jobId, jobA->toMessageId, clock->t));
    const auto sA = lc.snapshot(kA.toString());
    CHECK_EQ(sA.conversationKey, kA.toString());  // 结果归属 A
    CHECK_EQ(sA.lastSummarizedMessageId, static_cast<std::int64_t>(2));
    CHECK_TRUE(sA.runState == SummaryRunState::Idle);
    CHECK_FALSE(sA.summaryPending);

    const auto sB = lc.snapshot(kB.toString());
    CHECK_EQ(sB.conversationKey, kB.toString());
    CHECK_EQ(sB.lastSummarizedMessageId, static_cast<std::int64_t>(0));
    CHECK_TRUE(sB.summaryTaskId.empty());

    // 会话列表稳定有序
    const auto keys = lc.conversations();
    CHECK_EQ(keys.size(), static_cast<std::size_t>(2));
    CHECK_EQ(keys[0], kA.toString());
    CHECK_EQ(keys[1], kB.toString());
}

// ---------------------------------------------------------------------------
// T02：follower 到达 / 生成中 / 旧定时任务晚到
// ---------------------------------------------------------------------------
MIO_TEST(T02_follower与生成中不提前总结_旧任务晚到无效) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);
    archive.add(kA.toString(), 3);

    ConversationLifecycle lc(cfgOf(30, true, 1, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());

    const std::int64_t t0 = clock->t;
    lc.noteIncomingActivity(kA, t0);  // 有效消息 → 活动版本 +1
    CHECK_EQ(lc.snapshot(kA.toString()).activityVersion, static_cast<std::uint64_t>(1));

    clock->advance(29);
    CHECK_FALSE(lc.shouldSummarize(kA, clock->t).due);
    clock->advance(1);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);

    // InputBuffer follower 已接收、等待成批 → 不得提前总结
    lc.noteInputBuffered(kA, clock->t);
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("inputs_pending"));
        CHECK_FALSE(lc.tryBeginSummary(kA, clock->t).has_value());
        CHECK_EQ(lc.snapshot(kA.toString()).inFlightInputs, 1);
    }
    lc.noteInputConsumed(kA, clock->t);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);

    // 生成中 → 延迟摘要，不并发读取半成品对话
    lc.noteGenerationStart(kA, clock->t);
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("generation_active"));
        CHECK_FALSE(lc.tryBeginSummary(kA, clock->t).has_value());
    }

    // 生成结束算有效活动：静默窗口重新计时
    lc.noteGenerationEnd(kA, clock->t);
    const auto sAfterGen = lc.snapshot(kA.toString());
    CHECK_EQ(sAfterGen.activityVersion, static_cast<std::uint64_t>(2));
    CHECK_EQ(sAfterGen.lastActivityAt, clock->t);
    CHECK_EQ(sAfterGen.activeGenerations, 0);
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);  // 不提前总结
        CHECK_EQ(d.reason, std::string("silence_not_elapsed"));
        CHECK_FALSE(lc.tryBeginSummary(kA, clock->t).has_value());
    }

    clock->advance(30);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
    auto j1 = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(j1.has_value());
    CHECK_EQ(j1->attempt, 0);  // 首次尝试不计入重试
    CHECK_EQ(j1->activityVersion, static_cast<std::uint64_t>(2));

    // 同一会话最多一个有效任务 → 不得重复起任务
    CHECK_FALSE(lc.tryBeginSummary(kA, clock->t).has_value());
    CHECK_EQ(lc.snapshot(kA.toString()).summaryTaskId, j1->jobId);

    // 第一次失败 → 保留 pending 与重试次数，绝不标记为已总结
    CHECK_TRUE(lc.failSummary(j1->jobId, "模型超时", clock->t));
    const auto s1 = lc.snapshot(kA.toString());
    CHECK_TRUE(s1.summaryPending);
    CHECK_EQ(s1.retryCount, 1);
    CHECK_TRUE(s1.runState == SummaryRunState::Failed);
    CHECK_TRUE(s1.summaryTaskId.empty());
    CHECK_EQ(s1.lastSummarizedMessageId, static_cast<std::int64_t>(0));
    CHECK_EQ(s1.lastError, std::string("模型超时"));
    CHECK_TRUE(contains(s1.lastError, "SUMMARY_RETRY_EXHAUSTED") == false);

    clock->advance(1);
    auto j2 = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(j2.has_value());
    CHECK_EQ(j2->attempt, 1);
    // 相同范围重试：幂等唯一键不变（B 端"相同范围重试返回原记录"）
    CHECK_EQ(j2->uniqueKey(), j1->uniqueKey());

    // 旧定时任务晚到：activityVersion 已变化，旧任务的结果必须无效
    lc.noteIncomingActivity(kA, clock->t);  // follower 到达 → 版本 +1
    CHECK_EQ(lc.snapshot(kA.toString()).activityVersion, static_cast<std::uint64_t>(3));
    CHECK_FALSE(lc.completeSummary(j1->jobId, j1->toMessageId, clock->t));
    CHECK_FALSE(lc.failSummary(j1->jobId, "迟到的失败", clock->t));

    const auto s2 = lc.snapshot(kA.toString());
    CHECK_EQ(s2.lastSummarizedMessageId, static_cast<std::int64_t>(0));  // 没被旧任务推进
    CHECK_EQ(s2.retryCount, 1);                                          // 迟到失败不得加计
    CHECK_EQ(s2.summaryTaskId, j2->jobId);                               // 当前任务未被清掉
    CHECK_TRUE(s2.runState == SummaryRunState::Running);

    // 当前任务正常提交
    CHECK_TRUE(lc.completeSummary(j2->jobId, 3, clock->t));
    const auto s3 = lc.snapshot(kA.toString());
    CHECK_EQ(s3.lastSummarizedMessageId, static_cast<std::int64_t>(3));
    CHECK_EQ(s3.retryCount, 0);
    CHECK_TRUE(s3.runState == SummaryRunState::Idle);
    CHECK_FALSE(s3.summaryPending);
    // 重复的迟到完成回报同样无效（不会二次推进游标）
    CHECK_FALSE(lc.completeSummary(j2->jobId, 3, clock->t));
}

// ---------------------------------------------------------------------------
// T03：总结中收到新消息 → 冻结范围提交，新消息仍待总结
// ---------------------------------------------------------------------------
MIO_TEST(T03_总结中收到新消息_冻结范围提交_新消息仍待总结) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);

    // min=1：本轮只关心"新消息是否仍待总结"，阈值交互由 T04 覆盖
    ConversationLifecycle lc(cfgOf(30, true, 1, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());

    lc.noteIncomingActivity(kA, clock->t);
    clock->advance(30);
    auto job = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(job.has_value());
    CHECK_EQ(job->fromMessageId, static_cast<std::int64_t>(1));
    CHECK_EQ(job->toMessageId, static_cast<std::int64_t>(2));

    // 总结进行中：新消息已确认落盘并上报
    archive.add(kA.toString(), 3);
    lc.noteIncomingActivity(kA, clock->t);

    // 冻结范围成功提交：只能推进到冻结的结束 ID
    CHECK_TRUE(lc.completeSummary(job->jobId, job->toMessageId, clock->t));
    const auto s1 = lc.snapshot(kA.toString());
    CHECK_EQ(s1.lastSummarizedMessageId, static_cast<std::int64_t>(2));
    CHECK_TRUE(s1.summaryPending);   // 冻结之后的新消息仍然待总结
    CHECK_TRUE(s1.hasPendingMessages);
    CHECK_EQ(s1.pendingFromMessageId, static_cast<std::int64_t>(3));
    CHECK_EQ(s1.pendingCount, static_cast<std::int64_t>(1));
    CHECK_TRUE(s1.runState == SummaryRunState::Idle);

    // 新消息仍然遵守自己的静默窗口（不能被上一轮提交顺带清掉）
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("silence_not_elapsed"));
    }
    clock->advance(30);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
    auto job2 = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(job2.has_value());
    CHECK_EQ(job2->fromMessageId, static_cast<std::int64_t>(3));
    CHECK_EQ(job2->toMessageId, static_cast<std::int64_t>(3));
    CHECK_TRUE(lc.completeSummary(job2->jobId, 3, clock->t));
    const auto s2 = lc.snapshot(kA.toString());
    CHECK_EQ(s2.lastSummarizedMessageId, static_cast<std::int64_t>(3));
    CHECK_FALSE(s2.summaryPending);
}

MIO_TEST(T03_完成回报不得越过冻结终点) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);
    archive.add(kA.toString(), 3);
    archive.add(kB.toString(), 1);

    ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());

    lc.noteIncomingActivity(kA, clock->t);
    lc.noteIncomingActivity(kB, clock->t);
    clock->advance(10);

    auto jobA = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(jobA.has_value());
    CHECK_EQ(jobA->toMessageId, static_cast<std::int64_t>(3));
    // 传回比冻结终点更大的 ID：必须截断到冻结终点，不得越过
    CHECK_TRUE(lc.completeSummary(jobA->jobId, 999, clock->t));
    CHECK_EQ(lc.snapshot(kA.toString()).lastSummarizedMessageId, static_cast<std::int64_t>(3));

    // 未提供终点（<= 0）时按冻结终点提交
    auto jobB = lc.tryBeginSummary(kB, clock->t);
    CHECK_TRUE(jobB.has_value());
    CHECK_TRUE(lc.completeSummary(jobB->jobId, 0, clock->t));
    CHECK_EQ(lc.snapshot(kB.toString()).lastSummarizedMessageId, static_cast<std::int64_t>(1));

    // 未知任务 ID：忽略
    CHECK_FALSE(lc.completeSummary("silence-does-not-exist", 1, clock->t));
    CHECK_FALSE(lc.failSummary("silence-does-not-exist", "x", clock->t));
}

// ---------------------------------------------------------------------------
// T04：热更新
// ---------------------------------------------------------------------------
MIO_TEST(T04_非法热更新完整保留旧配置与调度行为) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);

    ConversationLifecycle lc(cfgOf(100, true, 2, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());
    lc.noteIncomingActivity(kA, clock->t);

    const std::vector<ConversationLifecycleConfig> bad = {
        cfgOf(0, true, 2, 3),      // 静默时间 < 1
        cfgOf(86401, true, 2, 3),  // 静默时间 > 86400
        cfgOf(100, true, -1, 3),   // 消息数为负
        cfgOf(100, true, 1001, 3), // 消息数越界
        cfgOf(100, true, 2, -1),   // 重试次数为负
        cfgOf(100, true, 2, 11),   // 重试次数越界
    };
    for (const auto& b : bad) {
        CHECK_FALSE(lc.configure(b));
        const auto cur = lc.config();
        // 整次失败：四个字段全部保持旧值，不得部分套用
        CHECK_EQ(cur.silenceTimeoutSeconds, 100);
        CHECK_TRUE(cur.summarizeOnSilence);
        CHECK_EQ(cur.minMessagesBeforeSummary, 2);
        CHECK_EQ(cur.maxPendingSummaryRetries, 3);
    }

    // 调度行为也确实没变：99 秒不到期，100 秒到期
    clock->advance(99);
    CHECK_FALSE(lc.shouldSummarize(kA, clock->t).due);
    clock->advance(1);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
}

MIO_TEST(T04_缩短延长阈值与静默开关) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);

    ConversationLifecycle lc(cfgOf(100, true, 1, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());
    lc.noteIncomingActivity(kA, clock->t);
    clock->advance(100);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);

    // 缩短阈值：立即（下一次查询）生效，不需要重建定时器
    CHECK_TRUE(lc.configure(cfgOf(10, true, 1, 3)));
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);

    // 延长阈值：重算 → 静默未到期
    CHECK_TRUE(lc.configure(cfgOf(300, true, 1, 3)));
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("silence_not_elapsed"));
    }

    // 关闭静默摘要：不得新起任务
    CHECK_TRUE(lc.configure(cfgOf(10, false, 1, 3)));
    clock->advance(1000);
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("summarize_disabled"));
        CHECK_FALSE(lc.tryBeginSummary(kA, clock->t).has_value());
    }

    // 重新开启：只处理未提交范围（游标仍为 0，没有重复处理"已总结"消息）
    CHECK_TRUE(lc.configure(cfgOf(10, true, 1, 3)));
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
    auto job = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(job.has_value());
    CHECK_EQ(job->fromMessageId, static_cast<std::int64_t>(1));
    CHECK_EQ(lc.snapshot(kA.toString()).lastSummarizedMessageId, static_cast<std::int64_t>(0));

    // 关闭静默摘要不影响已启动任务完成
    CHECK_TRUE(lc.configure(cfgOf(10, false, 1, 3)));
    CHECK_TRUE(lc.completeSummary(job->jobId, 1, clock->t));
    const auto s = lc.snapshot(kA.toString());
    CHECK_EQ(s.lastSummarizedMessageId, static_cast<std::int64_t>(1));
    CHECK_FALSE(s.summaryPending);
}

MIO_TEST(T04_消息阈值边界) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);  // count = 2
    archive.add(kB.toString(), 1);  // count = 1

    ConversationLifecycle lc(cfgOf(10, true, 3, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());
    lc.noteIncomingActivity(kA, clock->t);
    clock->advance(10);

    // min = 3，count = 2 → 不到期
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("below_message_threshold"));
        CHECK_FALSE(lc.tryBeginSummary(kA, clock->t).has_value());
    }
    // count = 3 → 到期
    archive.add(kA.toString(), 3);
    lc.noteIncomingActivity(kA, clock->t);
    clock->advance(10);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
    auto job = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(job.has_value());
    CHECK_EQ(job->fromMessageId, static_cast<std::int64_t>(1));
    CHECK_EQ(job->toMessageId, static_cast<std::int64_t>(3));

    // min = 0：不额外限制，但空范围仍不能总结
    CHECK_TRUE(lc.configure(cfgOf(10, true, 0, 3)));
    lc.noteIncomingActivity(kB, clock->t);
    clock->advance(10);
    CHECK_TRUE(lc.shouldSummarize(kB, clock->t).due);  // count = 1 即可总结
    auto jobB = lc.tryBeginSummary(kB, clock->t);
    CHECK_TRUE(jobB.has_value());

    // 只有活动、没有已确认落盘的可总结消息（空范围）→ 不得总结
    lc.noteIncomingActivity(kC, clock->t);
    clock->advance(10);
    {
        const DueDecision d = lc.shouldSummarize(kC, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("no_pending_messages"));
        CHECK_FALSE(lc.tryBeginSummary(kC, clock->t).has_value());
    }
}

// ---------------------------------------------------------------------------
// 重试耗尽
// ---------------------------------------------------------------------------
MIO_TEST(A_重试耗尽进入可观察失败状态并可人工复位) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);
    archive.add(kB.toString(), 1);

    ConversationLifecycle lc(cfgOf(10, true, 2, 1), clock, {});  // 只允许 1 次重试
    lc.setPendingRangeProvider(archive.provider());

    lc.noteIncomingActivity(kA, clock->t);
    clock->advance(10);
    auto j1 = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(j1.has_value());
    CHECK_TRUE(lc.failSummary(j1->jobId, "首次失败", clock->t));
    {
        const auto s = lc.snapshot(kA.toString());
        CHECK_EQ(s.retryCount, 1);
        CHECK_TRUE(s.runState == SummaryRunState::Failed);  // 还有重试额度
        CHECK_FALSE(contains(s.lastError, "SUMMARY_RETRY_EXHAUSTED"));
    }

    clock->advance(1);
    auto j2 = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(j2.has_value());
    CHECK_TRUE(lc.failSummary(j2->jobId, "第二次失败", clock->t));
    const auto sx = lc.snapshot(kA.toString());
    CHECK_EQ(sx.retryCount, 2);
    CHECK_TRUE(sx.runState == SummaryRunState::Exhausted);  // 可观察的失败状态
    CHECK_TRUE(contains(sx.lastError, "SUMMARY_RETRY_EXHAUSTED"));
    CHECK_TRUE(contains(sx.lastError, "HUMAN_REVIEW_REQUIRED"));  // 人工排障占位
    CHECK_TRUE(contains(sx.lastError, "第二次失败"));
    CHECK_TRUE(sx.summaryPending);  // 失败不得标记为已总结
    CHECK_EQ(sx.lastSummarizedMessageId, static_cast<std::int64_t>(0));
    CHECK_EQ(sx.frozenFromMessageId, static_cast<std::int64_t>(1));  // 冻结范围保留
    CHECK_EQ(sx.frozenToMessageId, static_cast<std::int64_t>(2));

    // 耗尽后即使静默到期、仍有未总结消息，也不得再自动起任务
    clock->advance(1000);
    {
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("run_exhausted"));
        CHECK_FALSE(lc.tryBeginSummary(kA, clock->t).has_value());
    }
    // 新消息也不会自动重置（重试额度只能由成功或人工复位归还）
    lc.noteIncomingActivity(kA, clock->t);
    CHECK_FALSE(lc.shouldSummarize(kA, clock->t).due);

    // 人工排障复位
    CHECK_TRUE(lc.clearFailure(kA, clock->t));
    const auto sr = lc.snapshot(kA.toString());
    CHECK_TRUE(sr.runState == SummaryRunState::Idle);
    CHECK_EQ(sr.retryCount, 0);
    CHECK_TRUE(sr.lastError.empty());
    clock->advance(10);
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
    CHECK_TRUE(lc.tryBeginSummary(kA, clock->t).has_value());

    // maxPendingSummaryRetries = 0：首次失败即耗尽
    ConversationLifecycle lc0(cfgOf(10, true, 1, 0), clock, {});
    lc0.setPendingRangeProvider(archive.provider());
    lc0.noteIncomingActivity(kB, clock->t);
    clock->advance(10);
    auto jb = lc0.tryBeginSummary(kB, clock->t);
    CHECK_TRUE(jb.has_value());
    CHECK_TRUE(lc0.failSummary(jb->jobId, "模型超时", clock->t));
    const auto sb = lc0.snapshot(kB.toString());
    CHECK_EQ(sb.retryCount, 1);
    CHECK_TRUE(sb.runState == SummaryRunState::Exhausted);
    CHECK_TRUE(contains(sb.lastError, "SUMMARY_RETRY_EXHAUSTED"));
    CHECK_FALSE(lc0.tryBeginSummary(kB, clock->t).has_value());
}

MIO_TEST(A_取消任务不消耗重试额度且可再调度) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);

    ConversationLifecycle lc(cfgOf(10, true, 1, 1), clock, {});
    lc.setPendingRangeProvider(archive.provider());
    lc.noteIncomingActivity(kA, clock->t);
    clock->advance(10);

    auto job = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(job.has_value());
    CHECK_TRUE(lc.cancelSummary(job->jobId, "应用关闭", clock->t));
    const auto s = lc.snapshot(kA.toString());
    CHECK_EQ(s.retryCount, 0);  // 取消不是模型失败，不消耗额度
    CHECK_TRUE(s.runState == SummaryRunState::Failed);
    CHECK_TRUE(s.summaryPending);
    CHECK_TRUE(s.summaryTaskId.empty());
    CHECK_FALSE(lc.cancelSummary(job->jobId, "重复取消", clock->t));  // 已失效

    clock->advance(1);
    CHECK_TRUE(lc.tryBeginSummary(kA, clock->t).has_value());
}

// ---------------------------------------------------------------------------
// 活动上报语义：待处理/生成上报不得反复延长静默时间
// ---------------------------------------------------------------------------
MIO_TEST(A_待处理与生成上报不延长静默时间) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);

    ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());

    const std::int64_t t0 = clock->t;
    lc.noteIncomingActivity(kA, t0);
    clock->advance(5);
    lc.noteInputBuffered(kA, clock->t);
    lc.noteInputConsumed(kA, clock->t);
    {
        const auto s = lc.snapshot(kA.toString());
        CHECK_EQ(s.activityVersion, static_cast<std::uint64_t>(1));  // 不是新活动
        CHECK_EQ(s.lastActivityAt, t0);                              // 没有延长静默窗
        CHECK_EQ(s.inFlightInputs, 0);
    }

    clock->advance(5);  // 距 t0 正好 10 秒
    CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);  // 待处理上报没有反复延长静默时间

    // 生成请求只挡并发，不改活动时间/版本
    lc.noteGenerationStart(kA, clock->t);
    {
        const auto s = lc.snapshot(kA.toString());
        CHECK_EQ(s.activityVersion, static_cast<std::uint64_t>(1));
        CHECK_EQ(s.lastActivityAt, t0);
        CHECK_EQ(s.activeGenerations, 1);
        const DueDecision d = lc.shouldSummarize(kA, clock->t);
        CHECK_FALSE(d.due);
        CHECK_EQ(d.reason, std::string("generation_active"));
    }

    // 只有模型输出完成才刷新活动时间
    lc.noteGenerationEnd(kA, clock->t);
    const auto s2 = lc.snapshot(kA.toString());
    CHECK_EQ(s2.activityVersion, static_cast<std::uint64_t>(2));
    CHECK_EQ(s2.lastActivityAt, clock->t);
    CHECK_FALSE(lc.shouldSummarize(kA, clock->t).due);
}

// ---------------------------------------------------------------------------
// 持久化
// ---------------------------------------------------------------------------
MIO_TEST(A_持久化_重试额度与冻结范围跨实例保留) {
    const auto dir = tempDir("persist");
    const auto path = dir / "conversation_lifecycle.json";
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);
    archive.add(kA.toString(), 2);

    {
        // max=1：只有一次重试额度，重启若刷新额度就不可能耗尽
        ConversationLifecycle lc(cfgOf(10, true, 1, 1), clock, path);
        lc.setPendingRangeProvider(archive.provider());
        lc.noteIncomingActivity(kA, clock->t);
        clock->advance(10);
        auto job = lc.tryBeginSummary(kA, clock->t);
        CHECK_TRUE(job.has_value());
        CHECK_TRUE(lc.failSummary(job->jobId, "第一次失败", clock->t));
        CHECK_TRUE(lc.persist());
    }
    CHECK_TRUE(std::filesystem::exists(path));

    // 落盘结构带 schema version，且不含瞬时计数
    {
        std::ifstream in(path);
        nlohmann::json doc;
        in >> doc;
        CHECK_EQ(doc["version"].get<int>(), 1);
        CHECK_TRUE(doc["conversations"].is_array());
        const auto& entry = doc["conversations"][0];
        CHECK_EQ(entry["conversationKey"].get<std::string>(), kA.toString());
        CHECK_EQ(entry["retryCount"].get<int>(), 1);
        CHECK_TRUE(entry["summaryPending"].get<bool>());
        CHECK_EQ(entry["runState"].get<std::string>(), std::string("failed"));
        CHECK_EQ(entry["frozenFromMessageId"].get<std::int64_t>(), static_cast<std::int64_t>(1));
        CHECK_EQ(entry["frozenToMessageId"].get<std::int64_t>(), static_cast<std::int64_t>(2));
        CHECK_FALSE(entry.contains("activeGenerations"));
    }

    {
        ConversationLifecycle lc(cfgOf(10, true, 1, 1), clock, path);
        CHECK_TRUE(lc.load());
        lc.setPendingRangeProvider(archive.provider());
        const auto s = lc.snapshot(kA.toString());
        CHECK_EQ(s.retryCount, 1);  // 重启不刷新重试额度
        CHECK_TRUE(s.summaryPending);
        CHECK_TRUE(s.runState == SummaryRunState::Failed);
        CHECK_EQ(s.lastSummarizedMessageId, static_cast<std::int64_t>(0));
        CHECK_EQ(s.lastError, std::string("第一次失败"));
        CHECK_EQ(s.frozenFromMessageId, static_cast<std::int64_t>(1));
        CHECK_EQ(s.frozenToMessageId, static_cast<std::int64_t>(2));
        CHECK_EQ(s.lastActivityAt, static_cast<std::int64_t>(1700000000));
        CHECK_EQ(s.activityVersion, static_cast<std::uint64_t>(1));

        // 只剩 1 次重试额度：再失败一次就耗尽（若额度被重启刷新，这里不会 Exhausted）
        clock->advance(1);
        auto job = lc.tryBeginSummary(kA, clock->t);
        CHECK_TRUE(job.has_value());
        CHECK_EQ(job->attempt, 1);
        CHECK_TRUE(lc.failSummary(job->jobId, "第二次失败", clock->t));
        const auto s2 = lc.snapshot(kA.toString());
        CHECK_TRUE(s2.runState == SummaryRunState::Exhausted);
        CHECK_TRUE(contains(s2.lastError, "SUMMARY_RETRY_EXHAUSTED"));
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MIO_TEST(A_持久化_崩溃恢复把在飞任务转为可重试) {
    const auto dir = tempDir("crash");
    const auto path = dir / "state.json";
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);

    {
        ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, path);
        lc.setPendingRangeProvider(archive.provider());
        lc.noteIncomingActivity(kA, clock->t);
        clock->advance(10);
        auto job = lc.tryBeginSummary(kA, clock->t);  // 任务在飞
        CHECK_TRUE(job.has_value());
        CHECK_TRUE(lc.persist());                     // 崩溃前最后一次落盘
    }
    {
        ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, path);
        CHECK_TRUE(lc.load());
        lc.setPendingRangeProvider(archive.provider());
        const auto s = lc.snapshot(kA.toString());
        // 新进程没有在飞任务的执行者：必须转为可重试状态，不能永久停在 Running
        CHECK_TRUE(s.runState == SummaryRunState::Failed);
        CHECK_TRUE(s.summaryTaskId.empty());
        CHECK_TRUE(contains(s.lastError, "未完成"));
        CHECK_EQ(s.retryCount, 0);  // 中断不消耗重试额度，但也不刷新额度
        CHECK_EQ(s.lastSummarizedMessageId, static_cast<std::int64_t>(0));

        clock->advance(1);
        CHECK_TRUE(lc.shouldSummarize(kA, clock->t).due);
        CHECK_TRUE(lc.tryBeginSummary(kA, clock->t).has_value());
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MIO_TEST(A_持久化_加载失败保持默认且不破坏原文件) {
    const auto dir = tempDir("broken");
    const auto path = dir / "broken.json";
    const std::string broken = "{ 这不是 JSON";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << broken;
    }

    auto clock = std::make_shared<FakeClock>();
    ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, path);
    lc.noteIncomingActivity(kA, clock->t);  // 已有内存状态
    CHECK_FALSE(lc.load());
    CHECK_EQ(lc.conversations().size(), static_cast<std::size_t>(1));  // 内存状态未被清空
    CHECK_EQ(readFile(path), broken);                                  // 原文件未被改写

    // version 不匹配 → 拒绝加载
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"version": 99, "conversations": []})";
    }
    CHECK_FALSE(lc.load());
    CHECK_EQ(lc.conversations().size(), static_cast<std::size_t>(1));

    // 文件不存在 → 保持默认（不报错、不创建文件）
    const auto missing = dir / "missing.json";
    ConversationLifecycle lc2(cfgOf(10, true, 1, 3), clock, missing);
    CHECK_FALSE(lc2.load());
    CHECK_TRUE(lc2.conversations().empty());
    CHECK_FALSE(std::filesystem::exists(missing));

    // 单条损坏：跳过该条，不影响其他会话
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"version": 1, "conversations": [
                    {"conversationKey": ""},
                    {"conversationKey": "private:OK", "retryCount": 2,
                     "runState": "exhausted", "summaryPending": true,
                     "lastSummarizedMessageId": 7, "lastActivityAt": 5,
                     "frozenFromMessageId": 3, "frozenToMessageId": 7}]})";
    }
    ConversationLifecycle lc3(cfgOf(10, true, 1, 3), clock, path);
    CHECK_TRUE(lc3.load());
    CHECK_EQ(lc3.conversations().size(), static_cast<std::size_t>(1));
    const auto s = lc3.snapshot("private:OK");
    CHECK_EQ(s.retryCount, 2);
    CHECK_TRUE(s.runState == SummaryRunState::Exhausted);
    CHECK_EQ(s.lastSummarizedMessageId, static_cast<std::int64_t>(7));
    CHECK_EQ(s.frozenToMessageId, static_cast<std::int64_t>(7));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MIO_TEST(A_持久化_原子写不留下临时文件) {
    const auto dir = tempDir("atomic");
    const auto path = dir / "state.json";
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.add(kA.toString(), 1);

    ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, path);
    lc.setPendingRangeProvider(archive.provider());
    lc.noteIncomingActivity(kA, clock->t);
    CHECK_TRUE(lc.persist());
    CHECK_TRUE(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")));

    // 未配置状态文件时 persist/load 是安全 no-op
    ConversationLifecycle mem(cfgOf(10, true, 1, 3), clock, {});
    CHECK_FALSE(mem.persist());
    CHECK_FALSE(mem.load());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 参与者：投影不给参与者时不得伪造，只在任务里留空提醒 Runtime 补齐
// ---------------------------------------------------------------------------
MIO_TEST(A_参与者缺失时不伪造只留空) {
    auto clock = std::make_shared<FakeClock>();
    FakeArchive archive;
    archive.withParticipants = false;
    archive.add(kA.toString(), 1);

    ConversationLifecycle lc(cfgOf(10, true, 1, 3), clock, {});
    lc.setPendingRangeProvider(archive.provider());
    lc.noteIncomingActivity(kA, clock->t);
    clock->advance(10);
    auto job = lc.tryBeginSummary(kA, clock->t);
    CHECK_TRUE(job.has_value());
    CHECK_TRUE(job->participants.empty());
    CHECK_TRUE(job->valid());  // 参与者由 Runtime 在投递前补齐
}
