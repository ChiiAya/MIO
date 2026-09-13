// ============================================================================
// 子任务 E 用例：MemoryProvider 本地后端适配 / unavailable 降级桩 / Hindsight 占位
//
// 覆盖文档验收项（docs/operations/memory-system-refactor.md）：
//   * T12 插件不可用：available()==false、storeEpisode 返回 Failed + STORAGE_UNAVAILABLE
//         （绝不 Stored/Queued）、recall 空列表且不抛；hindsight 与未知名称同样降级；
//         provider 析构无残留（无线程/无网络/无悬空访问）；
//   * 本地后端往返：storeEpisode → Stored + localId>0；同 AccessContext 可召回；
//         同范围重复提交 → Duplicate + 原 ID（不重复写）；伪造 localId 被拒绝；
//   * 三道可见性闸门：会话 B 查不到会话 A 的 conversation 记忆；Public 记录在
//         maxVisibility==Conversation 时不可见；跨会话（含 Public）一律不返回，
//         不泄漏条数与文本；
//   * context_compaction 默认不召回，allowContextCompaction=true 才召回；
//   * 长度限制：单条 ≤ promptEntryMaxBytes、总量 ≤ promptMaxBytes，UTF-8 安全截断
//         且截断可见；
//   * 失败不撒谎：非法范围/空会话/空正文/超长 → Rejected + 稳定错误码且不落库；
//         存储不可用 → Failed + STORAGE_UNAVAILABLE；
//   * 插件隔离：接口结构不含 reasoning 字段（编译期探针），returned 文本不含
//         非正文载荷；实现文件不依赖档案/图谱/文件系统头（E4 代码审查式断言）。
//
// 约束：数据库全部写到系统临时目录并在用例结束时删除，绝不读写真实 data/；
//       向量用 fake Embedding，不联网、不使用真实凭据、不引入新依赖。
// ============================================================================

#include "framework.h"

#include "context/summarizor/SummarySanitizer.h"  // isValidUtf8（仅测试断言用）
#include "core/contracts/Contracts.h"
#include "memory/manager/MemoryManager.h"
#include "memory/store/MemoryStore.h"
#include "providers/embedding/Embedding.h"
#include "providers/memory/HindsightMemoryProvider.h"
#include "providers/memory/LocalMemoryProvider.h"
#include "providers/memory/MemoryProviderFactory.h"
#include "providers/memory/UnavailableMemoryProvider.h"

#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace mio;

namespace {

// ---------------------------------------------------------------------------
// 临时目录（每个用例独立子目录；析构删除，含 -wal/-shm）
// ---------------------------------------------------------------------------
std::filesystem::path tempRoot() {
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() /
                 ("mio_test_e_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }();
    return dir;
}

class TempDb {
public:
    explicit TempDb(const std::string& name) {
        dir_ = tempRoot() / name;
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
        path_ = dir_ / "memory.db";
    }
    ~TempDb() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::remove(tempRoot(), ec);  // 非空时返回 false，无副作用
    }
    TempDb(const TempDb&) = delete;
    TempDb& operator=(const TempDb&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path dir_;
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// fake 向量后端：固定方向 [1,0,...] → 余弦相似度可预期（全 1.0）
// ---------------------------------------------------------------------------
class FakeEmbedding : public Embedding {
public:
    explicit FakeEmbedding(std::size_t dim = 4) : dim_(dim) {}
    std::vector<float> embed(const std::string& /*text*/) const override {
        ++calls;
        if (fail) throw std::runtime_error("fake embedding down");
        std::vector<float> v(dim_, 0.0f);
        v[0] = 1.0f;
        return v;
    }
    mutable int calls = 0;
    bool fail = false;

private:
    std::size_t dim_;
};

// ---------------------------------------------------------------------------
// 构造助手
// ---------------------------------------------------------------------------
EpisodeMemory makeEpisode(const std::string& conv, const std::string& person,
                          const std::string& text, Visibility vis, SummaryKind kind,
                          std::int64_t from, std::int64_t to, std::int64_t createdAt,
                          const std::string& source = "silence_summary") {
    EpisodeMemory e;
    e.conversationKey = conv;
    if (!person.empty()) e.participants.push_back(person);
    e.text = text;
    e.visibility = vis;
    e.kind = kind;
    e.fromMessageId = from;
    e.toMessageId = to;
    e.createdAt = createdAt;
    e.eventTime = createdAt;
    e.source = source;
    return e;
}

AccessContext makeAccess(const std::string& conv, const std::string& person,
                         Visibility maxVis, std::int64_t now) {
    AccessContext a;
    a.conversationKey = conv;
    a.requesterPersonId = person;
    a.maxVisibility = maxVis;
    a.now = now;
    return a;
}

RecallQuery makeQuery(const std::string& text, const AccessContext& access,
                      std::size_t topK = 3, bool allowCompaction = false) {
    RecallQuery q;
    q.text = text;
    q.access = access;
    q.topK = topK;
    q.allowContextCompaction = allowCompaction;
    q.now = access.now;
    return q;
}

bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void rawExec(const std::filesystem::path& db, const std::string& sql) {
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &h,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        if (h != nullptr) sqlite3_close(h);
        return;
    }
    char* err = nullptr;
    sqlite3_exec(h, sql.c_str(), nullptr, nullptr, &err);
    if (err != nullptr) sqlite3_free(err);
    sqlite3_close(h);
}

// 编译期探针：插件接口结构不得含 reasoning 字段（reasoning 永不进入插件输入/
// 输出；这是"接口层面拿不到 reasoning"的静态证据，而不是只靠运行时断言）。
template <typename T, typename = void>
struct HasReasoningMember : std::false_type {};
template <typename T>
struct HasReasoningMember<T, std::void_t<decltype(std::declval<T&>().reasoning)>>
    : std::true_type {};

// 代码审查断言：命中禁止依赖时报告具体文件与行号
void checkNoForbiddenToken(const std::string& file, int lineNo, const std::string& line,
                           const std::string& token) {
    ++::miotest::checks();
    if (has(line, token)) {
        ::miotest::fail(__FILE__, __LINE__,
                        file + ":" + std::to_string(lineNo) + " 出现禁止依赖/入口: " + token);
    }
}

// 定位源文件：兼容从仓库根目录或 build 输出目录运行的两种情况
std::string resolveSource(const std::string& rel) {
    const std::vector<std::string> prefixes = {"", "../", "../../", "../../../"};
    for (const auto& p : prefixes) {
        std::ifstream in(p + rel);
        if (in.good()) return p + rel;
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// T12：unavailable 桩不撒谎、不抛、不落盘
// ---------------------------------------------------------------------------
MIO_TEST(E_T12_unavailable_provider_never_claims_success) {
    TempDb db("t12_unavailable");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    MemoryManager mgr(cfg, emb, store);

    UnavailableMemoryProvider provider("hindsight 未接入");
    CHECK_EQ(std::string("unavailable"), provider.name());
    CHECK_FALSE(provider.available());

    const std::string conv = "console:private:E1201";
    const EpisodeMemory episode =
        makeEpisode(conv, "person-E12", "一条本应被记住的经历", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 5, 1000);
    const StoreEpisodeResult r = provider.storeEpisode(episode);
    CHECK_FALSE(r.ok());
    CHECK_TRUE(r.status == StoreStatus::Failed);
    CHECK_FALSE(r.status == StoreStatus::Stored);
    CHECK_FALSE(r.status == StoreStatus::Queued);
    CHECK_TRUE(r.code == ErrorCode::StorageUnavailable);
    CHECK_EQ(std::int64_t(0), r.localId);
    CHECK_FALSE(r.message.empty());

    const RecallQuery q = makeQuery("经历", makeAccess(conv, "person-E12",
                                                       Visibility::Conversation, 1100));
    CHECK_TRUE(provider.recall(q).empty());
    bool threw = false;
    try {
        const auto hits = provider.recall(q);
        CHECK_TRUE(hits.empty());
    } catch (...) {
        threw = true;
    }
    CHECK_FALSE(threw);

    // 不可用后端不得产生任何持久化痕迹
    CHECK_EQ(0u, mgr.count());
}

// ---------------------------------------------------------------------------
// T12：工厂选择后端；hindsight 与未知名称都降级且不抛
// ---------------------------------------------------------------------------
MIO_TEST(E_T12_factory_selects_backend_and_degrades_safely) {
    TempDb db("t12_factory");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    MemoryManager mgr(cfg, emb, store);

    const std::string conv = "console:private:E1202";
    const EpisodeMemory episode =
        makeEpisode(conv, "person-E12", "插件不可用时的经历", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 5, 1000);
    const RecallQuery q = makeQuery("经历", makeAccess(conv, "person-E12",
                                                       Visibility::Conversation, 1100));

    // 默认（空字符串）与显式 sqlite-local 都落到本地后端
    const auto dflt = makeMemoryProvider("", mgr, cfg);
    CHECK_TRUE(dflt != nullptr);
    CHECK_EQ(std::string("sqlite-local"), dflt->name());
    CHECK_TRUE(dflt->available());

    const auto local = makeMemoryProvider("sqlite-local", mgr, cfg);
    CHECK_EQ(std::string("sqlite-local"), local->name());
    CHECK_TRUE(local->available());

    // 显式 unavailable
    const auto unavailable = makeMemoryProvider("unavailable", mgr, cfg);
    CHECK_EQ(std::string("unavailable"), unavailable->name());
    CHECK_FALSE(unavailable->available());
    const auto ur = unavailable->storeEpisode(episode);
    CHECK_FALSE(ur.ok());
    CHECK_TRUE(ur.status == StoreStatus::Failed);
    CHECK_TRUE(ur.code == ErrorCode::StorageUnavailable);
    CHECK_TRUE(unavailable->recall(q).empty());

    // hindsight：占位实现，恒不可用；错误码为 HINDSIGHT_ADAPTER_TODO
    const auto hindsight = makeMemoryProvider("hindsight", mgr, cfg);
    CHECK_EQ(std::string("hindsight"), hindsight->name());
    CHECK_FALSE(hindsight->available());
    const auto hr = hindsight->storeEpisode(episode);
    CHECK_FALSE(hr.ok());
    CHECK_TRUE(hr.status == StoreStatus::Failed || hr.status == StoreStatus::Rejected);
    CHECK_FALSE(hr.status == StoreStatus::Stored);
    CHECK_FALSE(hr.status == StoreStatus::Queued);
    CHECK_TRUE(hr.code == ErrorCode::HindsightAdapterTodo);
    CHECK_EQ(std::string(kHindsightAdapterTodo), std::string(toString(hr.code)));
    CHECK_EQ(std::string("HINDSIGHT_ADAPTER_TODO"), std::string(toString(hr.code)));
    CHECK_EQ(std::int64_t(0), hr.localId);
    CHECK_TRUE(hindsight->recall(q).empty());
    // MemoryHit 无错误码字段：recall 的 TODO 状态用可观察的 lastError 暴露
    auto* concrete = dynamic_cast<HindsightMemoryProvider*>(hindsight.get());
    CHECK_TRUE(concrete != nullptr);
    if (concrete != nullptr) {
        CHECK_TRUE(has(concrete->lastError(), kHindsightAdapterTodo));
        CHECK_EQ(std::string(kHindsightAdapterTodo), std::string(concrete->adapterTodoCode()));
    }

    // 未知名称：降级为 unavailable（不抛异常、不回落到本地库）
    const auto unknown = makeMemoryProvider("hindsight-remote-v2", mgr, cfg);
    CHECK_TRUE(unknown != nullptr);
    CHECK_EQ(std::string("unavailable"), unknown->name());
    CHECK_FALSE(unknown->available());
    const auto xr = unknown->storeEpisode(episode);
    CHECK_FALSE(xr.ok());
    CHECK_TRUE(xr.status == StoreStatus::Failed);
    CHECK_TRUE(xr.code == ErrorCode::StorageUnavailable);
    CHECK_TRUE(unknown->recall(q).empty());

    // 所有降级路径都没有写入任何东西
    CHECK_EQ(0u, mgr.count());
}

// ---------------------------------------------------------------------------
// 本地后端往返：Stored + localId、召回字段映射、范围幂等、localId 幂等、topK
// ---------------------------------------------------------------------------
MIO_TEST(E_local_roundtrip_store_recall_and_idempotency) {
    TempDb db("local_roundtrip");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;  // topK=3 / minSimilarity=0.30（默认）
    MemoryManager mgr(cfg, emb, store);
    LocalMemoryProvider provider(mgr, cfg);

    CHECK_EQ(std::string("sqlite-local"), provider.name());
    CHECK_TRUE(provider.available());

    const std::string conv = "console:private:E1301";
    const std::string person = "person-E13";
    const std::string text = "用户和助手讨论了周末去山里徒步的计划。";
    const EpisodeMemory episode =
        makeEpisode(conv, person, text, Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 10, 1000);

    const StoreEpisodeResult stored = provider.storeEpisode(episode);
    CHECK_TRUE(stored.ok());
    CHECK_TRUE(stored.status == StoreStatus::Stored);
    CHECK_TRUE(stored.code == ErrorCode::Ok);
    CHECK_TRUE(stored.localId > 0);

    // 同一范围重复提交：Duplicate + 原 ID，不重复写
    const StoreEpisodeResult again = provider.storeEpisode(episode);
    CHECK_TRUE(again.ok());
    CHECK_TRUE(again.status == StoreStatus::Duplicate);
    CHECK_EQ(stored.localId, again.localId);
    CHECK_EQ(1u, mgr.count());

    // localId > 0（服务端已知记录）：幂等命中，不重复写
    EpisodeMemory known = episode;
    known.localId = stored.localId;
    const StoreEpisodeResult byId = provider.storeEpisode(known);
    CHECK_TRUE(byId.ok());
    CHECK_TRUE(byId.status == StoreStatus::Duplicate);
    CHECK_EQ(stored.localId, byId.localId);
    CHECK_EQ(1u, mgr.count());

    // 召回：字段映射逐项核对
    const AccessContext access =
        makeAccess(conv, person, Visibility::Conversation, 1300);
    const auto hits = provider.recall(makeQuery(text, access));
    CHECK_EQ(1u, hits.size());
    CHECK_EQ(stored.localId, hits[0].localId);
    CHECK_EQ(text, hits[0].text);
    CHECK_EQ(conv, hits[0].conversationKey);
    CHECK_TRUE(hits[0].visibility == Visibility::Conversation);
    CHECK_TRUE(hits[0].kind == SummaryKind::EpisodicMemory);
    CHECK_TRUE(hits[0].score > 0.0);
    CHECK_EQ(std::int64_t(1000), hits[0].createdAt);
    CHECK_EQ(std::string("silence_summary"), hits[0].source);

    // topK 生效：再写两条（不同范围），topK=2 只回 2 条
    CHECK_TRUE(provider
                   .storeEpisode(makeEpisode(conv, person, "第二段经历：聊了工作安排。",
                                             Visibility::Conversation,
                                             SummaryKind::EpisodicMemory, 11, 20, 1100))
                   .ok());
    CHECK_TRUE(provider
                   .storeEpisode(makeEpisode(conv, person, "第三段经历：聊了晚饭吃什么。",
                                             Visibility::Conversation,
                                             SummaryKind::EpisodicMemory, 21, 30, 1200))
                   .ok());
    CHECK_EQ(3u, mgr.count());
    const auto top2 = provider.recall(makeQuery("经历", access, /*topK=*/2));
    CHECK_EQ(2u, top2.size());
    const auto all3 = provider.recall(makeQuery("经历", access, /*topK=*/3));
    CHECK_EQ(3u, all3.size());

    // 空查询：直接空结果（不消耗 embedding、不返回任何记录）
    CHECK_TRUE(provider.recall(makeQuery("", access)).empty());
}

// ---------------------------------------------------------------------------
// context_compaction：可入库，但默认不作为经历召回
// ---------------------------------------------------------------------------
MIO_TEST(E_context_compaction_not_recalled_by_default) {
    TempDb db("compaction");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    MemoryManager mgr(cfg, emb, store);
    LocalMemoryProvider provider(mgr, cfg);

    const std::string conv = "console:private:E1401";
    const std::string person = "person-E14";

    const auto ep = provider.storeEpisode(
        makeEpisode(conv, person, "经历摘要：用户提到喜欢猫。", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 10, 1000));
    CHECK_TRUE(ep.status == StoreStatus::Stored);
    CHECK_EQ(10, mgr.committedCursor(conv));

    // compaction 仍可入库（可审计），但不推进静默游标、不默认召回
    const auto comp = provider.storeEpisode(
        makeEpisode(conv, person, "上下文压缩：本轮讨论了宠物话题。",
                    Visibility::Conversation, SummaryKind::ContextCompaction, 11, 20, 1100));
    CHECK_TRUE(comp.ok());
    CHECK_TRUE(comp.status == StoreStatus::Stored);
    CHECK_EQ(10, mgr.committedCursor(conv));
    auto compRecord = mgr.getSummary(comp.localId);
    CHECK_TRUE(compRecord.has_value());
    CHECK_TRUE(compRecord->kind == SummaryKind::ContextCompaction);
    CHECK_TRUE(compRecord->visibility == Visibility::Conversation);

    const AccessContext access =
        makeAccess(conv, person, Visibility::Conversation, 1300);
    const auto dflt = provider.recall(makeQuery("话题", access));
    CHECK_EQ(1u, dflt.size());
    CHECK_TRUE(dflt[0].kind == SummaryKind::EpisodicMemory);
    for (const auto& h : dflt) CHECK_FALSE(h.kind == SummaryKind::ContextCompaction);

    const auto withCompaction =
        provider.recall(makeQuery("话题", access, /*topK=*/3, /*allowCompaction=*/true));
    CHECK_EQ(2u, withCompaction.size());
    bool sawCompaction = false;
    for (const auto& h : withCompaction) {
        if (h.kind == SummaryKind::ContextCompaction) sawCompaction = true;
    }
    CHECK_TRUE(sawCompaction);
}

// ---------------------------------------------------------------------------
// 三道可见性闸门：跨会话不返回、Public 受 canSee 约束、不泄漏条数/文本
// ---------------------------------------------------------------------------
MIO_TEST(E_visibility_gates_block_cross_conversation_and_public) {
    TempDb db("visibility");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    cfg.writeVisibilityCap = Visibility::Public;  // 服务端显式允许写入 public
    MemoryManager mgr(cfg, emb, store);
    LocalMemoryProvider provider(mgr, cfg);

    const std::string convA = "console:private:E1501A";
    const std::string convB = "console:private:E1501B";
    const std::string personA = "person-E15A";
    const std::string personB = "person-E15B";
    const std::string secretA = "会话A的私密经历：银行卡尾号1234";
    const std::string publicA = "会话A标记为公开的经历：喜欢猫";

    const auto priv = provider.storeEpisode(
        makeEpisode(convA, personA, secretA, Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 5, 1000));
    CHECK_TRUE(priv.status == StoreStatus::Stored);
    const auto pub = provider.storeEpisode(
        makeEpisode(convA, personA, publicA, Visibility::Public,
                    SummaryKind::EpisodicMemory, 6, 9, 1100));
    CHECK_TRUE(pub.status == StoreStatus::Stored);
    auto pubRecord = mgr.getSummary(pub.localId);
    CHECK_TRUE(pubRecord.has_value());
    CHECK_TRUE(pubRecord->visibility == Visibility::Public);  // 上限允许时不被收窄

    // 会话 B（最窄上限）：查不到，且不泄漏条数/片段
    const auto bNarrow = provider.recall(
        makeQuery("经历", makeAccess(convB, personB, Visibility::Conversation, 1200)));
    CHECK_TRUE(bNarrow.empty());

    // 会话 B 即便上限=Public：跨会话闸门仍拦截（canAccessConversation 为假）
    const auto bWide =
        provider.recall(makeQuery("经历", makeAccess(convB, personB, Visibility::Public, 1200)));
    CHECK_TRUE(bWide.empty());
    for (const auto& h : bWide) {
        CHECK_FALSE(has(h.text, "1234"));
        CHECK_FALSE(has(h.text, secretA));
    }

    // 会话 A 最窄上限：conversation 记录可见，Public 记录被 canSee 拦截
    const auto aNarrow = provider.recall(
        makeQuery("经历", makeAccess(convA, personA, Visibility::Conversation, 1200)));
    CHECK_EQ(1u, aNarrow.size());
    CHECK_EQ(secretA, aNarrow[0].text);
    for (const auto& h : aNarrow) {
        CHECK_FALSE(has(h.text, publicA));
        CHECK_TRUE(h.visibility == Visibility::Conversation);
    }

    // 会话 A 上限=Public：两条都可见（闸门不是"一律拒绝"）
    const auto aWide =
        provider.recall(makeQuery("经历", makeAccess(convA, personA, Visibility::Public, 1200)));
    CHECK_EQ(2u, aWide.size());
    bool sawPublic = false;
    for (const auto& h : aWide) {
        if (h.visibility == Visibility::Public) sawPublic = true;
    }
    CHECK_TRUE(sawPublic);
}

// ---------------------------------------------------------------------------
// 长度限制：单条 ≤ promptEntryMaxBytes、总量 ≤ promptMaxBytes、UTF-8 安全截断
// ---------------------------------------------------------------------------
MIO_TEST(E_recall_respects_length_limits_and_truncates_utf8_safely) {
    TempDb db("length");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    MemoryManager mgr(cfg, emb, store);

    const std::string conv = "console:private:E1601";
    const std::string person = "person-E16";

    // 单条上限：中文正文（每字 3 字节）按 UTF-8 边界截断，不产生半个字符
    MemoryConfig entryCfg = cfg;
    entryCfg.promptEntryMaxBytes = 64;
    entryCfg.promptMaxBytes = 12000;
    LocalMemoryProvider entryProvider(mgr, entryCfg);

    std::string longChinese;
    for (int i = 0; i < 100; ++i) longChinese += "猫";  // 300 字节
    const auto one = entryProvider.storeEpisode(
        makeEpisode(conv, person, longChinese, Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 5, 1000));
    CHECK_TRUE(one.status == StoreStatus::Stored);

    const AccessContext access =
        makeAccess(conv, person, Visibility::Conversation, 1100);
    const auto oneHit = entryProvider.recall(makeQuery("猫", access));
    CHECK_EQ(1u, oneHit.size());
    CHECK_TRUE(oneHit[0].text.size() <= entryCfg.promptEntryMaxBytes);
    CHECK_TRUE(oneHit[0].text.size() < longChinese.size());  // 确实发生了截断
    CHECK_TRUE(isValidUtf8(oneHit[0].text));                 // 未切碎多字节字符
    CHECK_TRUE(has(oneHit[0].text, "已截断"));                // 截断可见

    // 总量上限：预算用尽即停止追加，单条与总量都不得越界
    MemoryConfig budgetCfg = cfg;
    budgetCfg.promptEntryMaxBytes = 64;
    budgetCfg.promptMaxBytes = 100;
    LocalMemoryProvider budgetProvider(mgr, budgetCfg);

    const std::string big(500, 'a');
    CHECK_TRUE(budgetProvider
                   .storeEpisode(makeEpisode(conv, person, big, Visibility::Conversation,
                                             SummaryKind::EpisodicMemory, 6, 10, 1100))
                   .ok());
    CHECK_TRUE(budgetProvider
                   .storeEpisode(makeEpisode(conv, person, big, Visibility::Conversation,
                                             SummaryKind::EpisodicMemory, 11, 15, 1200))
                   .ok());
    CHECK_TRUE(budgetProvider
                   .storeEpisode(makeEpisode(conv, person, big + "2", Visibility::Conversation,
                                             SummaryKind::EpisodicMemory, 16, 20, 1300))
                   .ok());

    const auto bounded = budgetProvider.recall(makeQuery("aaa", access, /*topK=*/3));
    CHECK_FALSE(bounded.empty());
    std::size_t total = 0;
    for (const auto& h : bounded) {
        CHECK_TRUE(h.text.size() <= budgetCfg.promptEntryMaxBytes);
        CHECK_TRUE(has(h.text, "已截断"));
        total += h.text.size();
    }
    CHECK_TRUE(total <= budgetCfg.promptMaxBytes);
    CHECK_EQ(std::size_t(2), bounded.size());  // 64 + 36 = 100，第三条装不下
    CHECK_EQ(budgetCfg.promptMaxBytes, total);
}

// ---------------------------------------------------------------------------
// 失败不撒谎：非法输入 Rejected、伪造 localId 拒绝、存储不可用 Failed
// ---------------------------------------------------------------------------
MIO_TEST(E_store_failures_never_report_success) {
    TempDb db("failures");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    MemoryManager mgr(cfg, emb, store);
    LocalMemoryProvider provider(mgr, cfg);

    const std::string conv = "console:private:E1701";
    const std::string person = "person-E17";

    // 非法范围（fromMessageId=0）：Rejected + INVALID_ARGUMENT，且不伪造 ID
    const auto zeroRange = provider.storeEpisode(
        makeEpisode(conv, person, "范围缺失的经历", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 0, 0, 1000));
    CHECK_FALSE(zeroRange.ok());
    CHECK_TRUE(zeroRange.status == StoreStatus::Rejected);
    CHECK_TRUE(zeroRange.code == ErrorCode::InvalidArgument);
    CHECK_EQ(std::int64_t(0), zeroRange.localId);

    // from > to
    const auto reversed = provider.storeEpisode(
        makeEpisode(conv, person, "范围颠倒的经历", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 9, 5, 1000));
    CHECK_FALSE(reversed.ok());
    CHECK_TRUE(reversed.status == StoreStatus::Rejected);
    CHECK_TRUE(reversed.code == ErrorCode::InvalidArgument);

    // 空会话 / 空正文
    const auto noConv = provider.storeEpisode(
        makeEpisode("", person, "无会话归属", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 5, 1000));
    CHECK_TRUE(noConv.status == StoreStatus::Rejected);
    CHECK_TRUE(noConv.code == ErrorCode::InvalidArgument);
    const auto emptyText = provider.storeEpisode(
        makeEpisode(conv, person, "", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 5, 1000));
    CHECK_TRUE(emptyText.status == StoreStatus::Rejected);
    CHECK_TRUE(emptyText.code == ErrorCode::InvalidArgument);

    // 超长写入：超限返回 LIMIT_EXCEEDED（不静默截断、不假装成功）
    const auto tooLong = provider.storeEpisode(
        makeEpisode(conv, person, std::string(limits::kWriteMaxBytes + 1, 'x'),
                    Visibility::Conversation, SummaryKind::EpisodicMemory, 1, 5, 1000));
    CHECK_FALSE(tooLong.ok());
    CHECK_TRUE(tooLong.status == StoreStatus::Rejected);
    CHECK_TRUE(tooLong.code == ErrorCode::LimitExceeded);

    // 以上全部失败：库里没有任何行
    CHECK_EQ(0u, mgr.count());

    // 伪造 localId：指向不存在的记录 → Rejected，不伪装成 Duplicate
    EpisodeMemory forged = makeEpisode(conv, person, "伪造 ID 的经历", Visibility::Conversation,
                                       SummaryKind::EpisodicMemory, 1, 5, 1000);
    forged.localId = 999999;
    const auto forgedResult = provider.storeEpisode(forged);
    CHECK_TRUE(forgedResult.status == StoreStatus::Rejected);
    CHECK_TRUE(forgedResult.code == ErrorCode::InvalidArgument);
    CHECK_EQ(0u, mgr.count());

    // 跨会话伪造 localId：拒绝
    const auto good = provider.storeEpisode(
        makeEpisode("console:private:E1701-other", person, "另一会话的真实经历",
                    Visibility::Conversation, SummaryKind::EpisodicMemory, 1, 5, 1000));
    CHECK_TRUE(good.status == StoreStatus::Stored);
    EpisodeMemory crossConv = makeEpisode(conv, person, "跨会话冒领", Visibility::Conversation,
                                          SummaryKind::EpisodicMemory, 1, 5, 1000);
    crossConv.localId = good.localId;
    const auto crossResult = provider.storeEpisode(crossConv);
    CHECK_TRUE(crossResult.status == StoreStatus::Rejected);
    CHECK_TRUE(crossResult.code == ErrorCode::InvalidArgument);

    // 存储不可用（表被破坏）：Failed + STORAGE_UNAVAILABLE，绝不 Stored/Queued
    rawExec(db.path(), "DROP TABLE memories");
    const auto down = provider.storeEpisode(
        makeEpisode(conv, person, "存储不可用时的经历", Visibility::Conversation,
                    SummaryKind::EpisodicMemory, 1, 5, 1000));
    CHECK_FALSE(down.ok());
    CHECK_TRUE(down.status == StoreStatus::Failed);
    CHECK_FALSE(down.status == StoreStatus::Stored);
    CHECK_FALSE(down.status == StoreStatus::Queued);
    CHECK_TRUE(down.code == ErrorCode::StorageUnavailable);
    CHECK_EQ(std::int64_t(0), down.localId);
    CHECK_FALSE(down.message.empty());

    // 后端异常内部消化：召回返回空列表且不抛出
    bool threw = false;
    try {
        const auto hits = provider.recall(
            makeQuery("经历", makeAccess(conv, person, Visibility::Conversation, 1200)));
        CHECK_TRUE(hits.empty());
    } catch (...) {
        threw = true;
    }
    CHECK_FALSE(threw);
}

// ---------------------------------------------------------------------------
// T12（后半）：provider 无线程/无网络，析构即回收，不留悬空访问
// ---------------------------------------------------------------------------
MIO_TEST(E_T12_provider_destruction_has_no_threads_or_dangling_state) {
    TempDb db("teardown");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    MemoryManager mgr(cfg, emb, store);

    const std::string conv = "console:private:E1801";
    const std::string person = "person-E18";
    {
        auto provider = makeMemoryProvider("sqlite-local", mgr, cfg);
        const auto r = provider->storeEpisode(
            makeEpisode(conv, person, "关机前写入的经历", Visibility::Conversation,
                        SummaryKind::EpisodicMemory, 1, 5, 1000));
        CHECK_TRUE(r.status == StoreStatus::Stored);
    }  // provider 析构：本实现无后台线程/无网络连接，析构即回收

    // 依赖（MemoryManager/MemoryStore）由组合根持有：析构 provider 后仍可正常读写
    CHECK_EQ(1u, mgr.count());
    const auto after = mgr.writeSummary(
        [&] {
            SummaryRecord rec;
            rec.conversationKey = conv;
            rec.participants = {person};
            rec.kind = SummaryKind::EpisodicMemory;
            rec.summary = "关机后由别处写入的经历";
            rec.visibility = Visibility::Conversation;
            rec.createdAt = 2000;
            rec.eventTime = 2000;
            rec.fromMessageId = 6;
            rec.toMessageId = 9;
            rec.source = "silence_summary";
            return rec;
        }());
    CHECK_TRUE(after.ok);
    CHECK_EQ(2u, mgr.count());
}

// ---------------------------------------------------------------------------
// 插件隔离（接口层）：输入/输出结构不含 reasoning 字段；返回文本只有正文
// ---------------------------------------------------------------------------
MIO_TEST(E_plugin_surface_cannot_carry_reasoning) {
    // 编译期探针：reasoning 在冻结节构里没有落脚点（不是"运行时恰好没出现"）
    CHECK_FALSE(HasReasoningMember<EpisodeMemory>::value);
    CHECK_FALSE(HasReasoningMember<MemoryHit>::value);
    CHECK_FALSE(HasReasoningMember<RecallQuery>::value);
    CHECK_FALSE(HasReasoningMember<StoreEpisodeResult>::value);

    TempDb db("reasoning");
    MemoryStore store(db.path());
    auto emb = std::make_shared<FakeEmbedding>(4);
    MemoryConfig cfg;
    cfg.dim = 4;
    MemoryManager mgr(cfg, emb, store);
    LocalMemoryProvider provider(mgr, cfg);

    const std::string conv = "console:private:E1901";
    const std::string person = "person-E19";
    const std::string marker = "REASONING-INTERNAL-MARKER";
    const std::string body = "用户喜欢猫，家里养了两只。";

    EpisodeMemory episode = makeEpisode(conv, person, body, Visibility::Conversation,
                                        SummaryKind::EpisodicMemory, 1, 5, 1000);
    // 把 reasoning 风格载荷塞进正文之外的字段：它们不得出现在召回文本里
    episode.idempotencyKey = marker;
    episode.participants.push_back(marker + "-participant");

    const auto stored = provider.storeEpisode(episode);
    CHECK_TRUE(stored.status == StoreStatus::Stored);

    const auto hits = provider.recall(makeQuery(
        "猫", makeAccess(conv, person, Visibility::Conversation, 1100)));
    CHECK_EQ(1u, hits.size());
    CHECK_EQ(body, hits[0].text);                       // 正文逐字一致，无附加内容
    CHECK_FALSE(has(hits[0].text, marker));             // 非正文载荷不进入返回文本
    CHECK_FALSE(has(hits[0].text, "reasoning"));
    CHECK_FALSE(has(hits[0].text, "REASONING"));
}

// ---------------------------------------------------------------------------
// 插件隔离（实现层）：providers/memory/** 不依赖档案/图谱/文件系统入口
// ---------------------------------------------------------------------------
MIO_TEST(E_plugin_sources_touch_no_archive_or_graph_files) {
    // 白名单断言：本地适配器声明的依赖只有 MemoryManager / MemoryProvider / 契约 / 日志
    const auto& localAllowed = LocalMemoryProvider::allowedIncludes();
    CHECK_EQ(std::size_t(4), localAllowed.size());
    CHECK_TRUE(std::find(localAllowed.begin(), localAllowed.end(),
                         "memory/manager/MemoryManager.h") != localAllowed.end());
    CHECK_TRUE(std::find(localAllowed.begin(), localAllowed.end(),
                         "providers/memory/MemoryProvider.h") != localAllowed.end());
    CHECK_TRUE(std::find(localAllowed.begin(), localAllowed.end(),
                         "core/contracts/Limits.h") != localAllowed.end());

    const std::vector<std::string> allowed = {
        "core/contracts/Errors.h",
        "core/contracts/Limits.h",
        "log/Log.h",
        "memory/manager/MemoryManager.h",
        "providers/memory/HindsightMemoryProvider.h",
        "providers/memory/LocalMemoryProvider.h",
        "providers/memory/MemoryProvider.h",
        "providers/memory/MemoryProviderFactory.h",
        "providers/memory/UnavailableMemoryProvider.h",
    };
    // 禁止的代码入口/标识（不含注释里"不得读取"的中文说明；这是审查辅助，
    // 不是沙箱：真正的隔离需要进程边界）
    const std::vector<std::string> forbidden = {
        "Achieve",       "RelationshipGraph", "relationships.json", "jsonl",
        "ifstream",      "ofstream",          "fstream",            "filesystem",
        "opendir",       "fopen",             "getenv",
    };
    const std::vector<std::string> files = {
        "src/providers/memory/LocalMemoryProvider.h",
        "src/providers/memory/LocalMemoryProvider.cpp",
        "src/providers/memory/UnavailableMemoryProvider.h",
        "src/providers/memory/UnavailableMemoryProvider.cpp",
        "src/providers/memory/HindsightMemoryProvider.h",
        "src/providers/memory/HindsightMemoryProvider.cpp",
        "src/providers/memory/MemoryProviderFactory.h",
        "src/providers/memory/MemoryProviderFactory.cpp",
    };

    int scanned = 0;
    for (const auto& rel : files) {
        const std::string path = resolveSource(rel);
        if (path.empty()) continue;
        std::ifstream in(path);
        if (!in.good()) continue;
        ++scanned;
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            // 只允许白名单里的引号 include（标准库尖括号 include 不计）
            if (has(line, "#include") && has(line, "\"")) {
                const std::size_t b = line.find('"');
                const std::size_t e = line.find('"', b + 1);
                if (e != std::string::npos && e > b) {
                    const std::string inc = line.substr(b + 1, e - b - 1);
                    ++::miotest::checks();
                    if (std::find(allowed.begin(), allowed.end(), inc) == allowed.end()) {
                        ::miotest::fail(__FILE__, __LINE__,
                                        rel + ":" + std::to_string(lineNo) +
                                            " 依赖了白名单外的头文件: " + inc);
                    }
                }
            }
            for (const auto& bad : forbidden) checkNoForbiddenToken(rel, lineNo, line, bad);
        }
    }
    if (scanned == 0) {
        std::fprintf(stderr,
                     "  [WARN] 未找到 providers/memory 源文件（工作目录不同？），"
                     "E4 文本审查跳过；白名单断言已执行\n");
    }
    CHECK_TRUE(scanned == 0 || scanned == static_cast<int>(files.size()));

    // 头文件不提供沙箱：接口约束不等于进程隔离。该边界写在实现注释与报告中，
    // 这里只断言占位后端仍按"未接入"语义工作，避免文本审查给人"已隔离"的错觉。
    HindsightMemoryProvider placeholder;
    CHECK_EQ(std::string("hindsight"), placeholder.name());
    CHECK_FALSE(placeholder.available());
}
