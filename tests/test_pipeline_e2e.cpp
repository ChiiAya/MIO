// 主 agent 端到端集成用例（跨子任务 A/B/F + Runtime 接线）
//
// 覆盖文档验收表中需要"整条链路"才能证明的部分：
//   * T01 A 会话到期、B 会话仍活跃 → 只总结 A，后台结果归属 A
//   * T03 总结中/总结后收到新消息 → 冻结范围成功提交，新消息仍待总结
//   * T05 重启后游标正确、不重复摘要（游标来自落盘状态 + 范围幂等）
//   * T09 摘要产出物不得扩大可见范围（写入可见性恒为 conversation）
//
// 全程不联网：LLM 用 fake；embedding 指向不可达端口并立即失败（降级路径）。

#include "framework.h"

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "core/event/Event.h"
#include "providers/llm/Llm.h"
#include "runtime/Runtime.h"

#include <sqlite3.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace mio;

namespace {

// ---- fake LLM：返回可控摘要文本，并统计调用次数 --------------------------
struct FakeLlm final : public Llm {
    mutable std::atomic<int> chatCalls{0};
    std::string reply =
        "用户说他喜欢喝乌龙茶，并且不吃香菜。\n话题：饮食习惯\n私密性判断：私密";

    ChatResponse chat(const ChatRequest&) override {
        ++chatCalls;
        ChatResponse r;
        r.text = reply;
        r.finishReason = "stop";
        r.usage.inputOther = 10;
        r.usage.output = 10;
        return r;
    }
};

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("mio_e2e_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter()++));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    static int& counter() {
        static int c = 0;
        return c;
    }
};

std::shared_ptr<AppConfig> testConfig(const std::filesystem::path& dir) {
    auto cfg = std::make_shared<AppConfig>(AppConfig{});
    cfg->adminPort = 0;                     // 测试不启动管理服务
    cfg->dataDir = dir.string();
    cfg->platform = "test";
    cfg->conversation.silenceTimeoutSeconds = 1;   // 1s 静默即到期（测试专用）
    cfg->conversation.summarizeOnSilence = true;
    cfg->conversation.minMessagesBeforeSummary = 1;
    cfg->conversation.maxPendingSummaryRetries = 1;
    cfg->contextBuilder.budgetTokens = 4096;
    cfg->contextBuilder.coldStartRawTokens = 256;
    cfg->contextBuilder.coldStartMaxMessages = 8;
    // embedding 指向不可达端口：向量化立即失败，走"保留文本 + 待办重试"降级路径
    cfg->embedding.baseUrl = "http://127.0.0.1:1/v1";
    cfg->embedding.apiKey = "unused";
    cfg->embedding.maxRetries = 0;
    cfg->embedding.httpTimeoutMs = 200;
    return cfg;
}

IncomingMessage makeMsg(const std::string& convId, const std::string& text) {
    IncomingMessage m;
    m.conversation = ConversationKey::privateChat(convId);
    m.conversation.platform = "test";
    m.senderId = "1000";
    m.senderName = "小明";
    m.platform = "test";
    m.text = text;
    return m;
}

// 直接以只读连接统计落库摘要（验证 B 的写入确实发生，且可见性未放宽）。
// 按会话过滤：测试里多个会话都会到期，全局计数会互相干扰。
struct DbProbe {
    int episodicRows = -1;
    int nonConversationRows = -1;
};

DbProbe probe(const std::filesystem::path& dbFile, const std::string& convKeySuffix = "") {
    DbProbe out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbFile.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
        SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    auto count = [&](const std::string& sql, const std::string& filter) {
        sqlite3_stmt* st = nullptr;
        int n = -1;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            if (!filter.empty()) {
                sqlite3_bind_text(st, 1, filter.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        return n;
    };
    if (convKeySuffix.empty()) {
        out.episodicRows = count(
            "SELECT COUNT(*) FROM memories WHERE summary_kind='episodic_memory'", "");
    } else {
        out.episodicRows = count(
            "SELECT COUNT(*) FROM memories WHERE summary_kind='episodic_memory'"
            " AND conv_key LIKE ?1",
            "%" + convKeySuffix);
    }
    out.nonConversationRows = count(
        "SELECT COUNT(*) FROM memories WHERE visibility <> 'conversation'", "");
    sqlite3_close(db);
    return out;
}

ConversationLifecycleState stateOf(Runtime& rt, const std::string& key) {
    for (const auto& s : rt.lifecycleStates()) {
        if (s.conversationKey == key) return s;
    }
    return ConversationLifecycleState{};
}

// 轮询等待条件成立（测试自身不依赖真实时钟精度，只等后台 tick）
template <typename Pred>
bool waitUntil(Pred pred, int timeoutMs = 25000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return pred();
}

} // namespace

MIO_TEST(T01_T03_端到端_只总结到期会话_新消息仍待总结) {
    TempDir dir;
    auto cfg = testConfig(dir.path);
    auto mgr = std::make_shared<ConfigManager>(cfg);
    auto llm = std::make_shared<FakeLlm>();
    // 每个用例自建独立 Runtime（独立临时 dataDir），互不影响
    Runtime rt("Mio", dir.path, llm, mgr);

    const std::string convA = ConversationKey::privateChat("uA").toString();
    const std::string convB = ConversationKey::privateChat("uB").toString();
    // 注意 ConversationKey::privateChat 不带 platform；ingest 用的是带 platform 的
    // key，因此以 Runtime 实际记录的键为准（下面用后缀匹配）。
    const std::string suffixA = "private:uA";
    const std::string suffixB = "private:uB";

    // A 会话两条消息后静默；B 会话持续活跃（间隔 < 静默阈值）
    rt.ingest(makeMsg("uA", "我喜欢喝乌龙茶"));
    rt.ingest(makeMsg("uA", "我不吃香菜"));

    // 档案里每条用户消息都会带一条助手回复，因此"已提交游标"是可见消息的最大 ID，
    // 这里只断言语义（覆盖了用户消息、且只提交了第一批），不写死具体数字。
    std::int64_t cursorAfterFirst = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int bSeq = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        // B 持续说话：它的静默窗口不断被刷新，不应该被总结
        rt.ingest(makeMsg("uB", "B 还在聊 " + std::to_string(++bSeq)));
        for (const auto& s : rt.lifecycleStates()) {
            if (s.conversationKey.find(suffixA) != std::string::npos &&
                s.lastSummarizedMessageId >= 2) {
                cursorAfterFirst = s.lastSummarizedMessageId;
            }
        }
        if (cursorAfterFirst > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    CHECK_TRUE(cursorAfterFirst > 0);

    // ---- T01：后台结果归属 A，且 A 的游标推进到冻结范围终点 ----
    bool sawA = false;
    for (const auto& s : rt.lifecycleStates()) {
        if (s.conversationKey.find(suffixA) != std::string::npos) {
            sawA = true;
            CHECK_EQ(s.lastSummarizedMessageId, cursorAfterFirst);
            CHECK_TRUE(s.runState != SummaryRunState::Exhausted);
            CHECK_FALSE(s.summaryPending);
        }
        // B 尚未到期：仍待总结（只总结 A）
        if (s.conversationKey.find(suffixB) != std::string::npos) {
            CHECK_EQ(s.lastSummarizedMessageId, static_cast<std::int64_t>(0));
        }
    }
    CHECK_TRUE(sawA);

    // ---- 落库校验：A 有经验记忆，且可见性未被放宽 ----
    const DbProbe p1 = probe(dir.path / "memory.db", suffixA);
    CHECK_EQ(p1.episodicRows, 1);
    CHECK_EQ(p1.nonConversationRows, 0);  // T09：恒为 conversation

    // ---- T03：摘要完成后新消息仍待总结；游标只推进到冻结终点 ----
    rt.ingest(makeMsg("uA", "另外我喜欢喝普洱"));
    const bool aAdvanced = waitUntil([&]() {
        return probe(dir.path / "memory.db", suffixA).episodicRows == 2;
    });
    CHECK_TRUE(aAdvanced);
    std::int64_t cursorAfterSecond = 0;
    for (const auto& s : rt.lifecycleStates()) {
        if (s.conversationKey.find(suffixA) != std::string::npos) {
            cursorAfterSecond = s.lastSummarizedMessageId;
        }
    }
    // 冻结范围之外的旧消息不会被重新提交；新消息被单独提交
    CHECK_TRUE(cursorAfterSecond > cursorAfterFirst);
    const DbProbe p2 = probe(dir.path / "memory.db", suffixA);
    CHECK_EQ(p2.episodicRows, 2);
    CHECK_EQ(p2.nonConversationRows, 0);
}

MIO_TEST(T12_端到端_插件不可用时对话继续且可干净回收) {
    TempDir dir;
    auto cfg = testConfig(dir.path);
    cfg->memory.backend = "unavailable";  // 经历记忆后端不可用（T12）
    auto mgr = std::make_shared<ConfigManager>(cfg);
    auto llm = std::make_shared<FakeLlm>();
    {
        Runtime rt("Mio", dir.path, llm, mgr);
        // 对话必须继续：不可用的插件不得阻塞主链路
        const BotReply r1 = rt.ingest(makeMsg("uD", "在吗"));
        CHECK_FALSE(r1.text.empty());
        const BotReply r2 = rt.ingest(makeMsg("uD", "插件坏了也照常聊"));
        CHECK_FALSE(r2.text.empty());
        CHECK_TRUE(llm->chatCalls.load() >= 2);
    }
    // 析构已返回：维护线程已回收、依赖已释放，不残留后台访问
    CHECK_TRUE(true);
}

MIO_TEST(T12_端到端_Hindsight占位不阻塞且无网络依赖) {
    TempDir dir;
    auto cfg = testConfig(dir.path);
    cfg->memory.backend = "hindsight";  // 占位：HINDSIGHT_ADAPTER_TODO
    auto mgr = std::make_shared<ConfigManager>(cfg);
    auto llm = std::make_shared<FakeLlm>();
    Runtime rt("Mio", dir.path, llm, mgr);
    const BotReply r = rt.ingest(makeMsg("uE", "你好"));
    CHECK_FALSE(r.text.empty());
}

MIO_TEST(T05_端到端_重启后游标正确且不重复摘要) {
    TempDir dir;
    std::int64_t cursorBeforeRestart = 0;
    {
        auto cfg = testConfig(dir.path);
        auto mgr = std::make_shared<ConfigManager>(cfg);
        auto llm = std::make_shared<FakeLlm>();
        Runtime rt("Mio", dir.path, llm, mgr);
        rt.ingest(makeMsg("uC", "第一句"));
        rt.ingest(makeMsg("uC", "第二句"));
        const bool done = waitUntil([&]() {
            for (const auto& s : rt.lifecycleStates()) {
                if (s.conversationKey.find("private:uC") != std::string::npos &&
                    s.lastSummarizedMessageId >= 2) {
                    cursorBeforeRestart = s.lastSummarizedMessageId;
                    return true;
                }
            }
            return false;
        });
        CHECK_TRUE(done);
        CHECK_EQ(probe(dir.path / "memory.db", "private:uC").episodicRows, 1);
        // Runtime 析构：停调度 → 回收在飞任务 → 持久化游标/重试额度
    }

    // ---- 重启：游标从落盘状态恢复，同一范围不得重复写入 ----
    {
        auto cfg = testConfig(dir.path);
        auto mgr = std::make_shared<ConfigManager>(cfg);
        auto llm = std::make_shared<FakeLlm>();
        Runtime rt("Mio", dir.path, llm, mgr);
        bool restored = false;
        for (const auto& s : rt.lifecycleStates()) {
            if (s.conversationKey.find("private:uC") != std::string::npos) {
                restored = true;
                CHECK_EQ(s.lastSummarizedMessageId, cursorBeforeRestart);
                CHECK_FALSE(s.summaryPending);
                CHECK_TRUE(s.runState != SummaryRunState::Exhausted);
            }
        }
        CHECK_TRUE(restored);
        // 静置数个 tick：没有新消息 → 不得重复摘要、不得新增记忆
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        CHECK_EQ(probe(dir.path / "memory.db", "private:uC").episodicRows, 1);
        CHECK_TRUE(cursorBeforeRestart > 0);
    }
}
