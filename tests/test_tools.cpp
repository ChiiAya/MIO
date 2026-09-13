// ============================================================================
// 子任务 D 用例：模型记忆与会话查询工具（handler 层）
//
// 覆盖文档验收项：
//   * envelope 形状 {ok,data,error} 与稳定错误码（字符串断言）；
//   * T09 越权：非当前会话 → NOT_FOUND_OR_FORBIDDEN 且不泄漏其他会话内容；
//     伪造/跨会话/跨查询游标 → INVALID_ARGUMENT；
//   * 参数过大：读取截断 + 游标、单次 ≤ 12000 字节；写入 > 4000 → LIMIT_EXCEEDED；
//     query > 1000 → LIMIT_EXCEEDED；
//   * T08 请求批准 → HUMAN_REVIEW_REQUIRED 且状态不变；伪造审核人字段 →
//     INVALID_ARGUMENT；提案后 FactStore::listConfirmed 不增加；
//     memory_list_pending 在 adminChannel=false 时 NOT_FOUND_OR_FORBIDDEN；
//   * T10 manual 幂等 + 不推进静默总结游标；
//   * 写工具返回 ID + 状态（不是自然语言"已记住"）；
//   * 不返回 reasoning、不返回完整工具参数；
//   * 注册名是下划线形式，且不含 approve/reject/set_public 工具。
//
// 约束：全部使用系统临时目录；不读写真实 data/、不联网、不用付费接口。
// ============================================================================

#include "framework.h"

#include "context/achieve/Achieve.h"
#include "core/contracts/Contracts.h"
#include "memory/manager/MemoryManager.h"
#include "memory/store/MemoryStore.h"
#include "mind/graph/RelationshipGraph.h"
#include "mind/proposals/CognitionStore.h"
#include "mind/proposals/FactStore.h"
#include "mind/proposals/ProposalStore.h"
#include "providers/embedding/Embedding.h"
#include "providers/llm/tool/ToolRegistry.h"
#include "providers/llm/tool/handlers/MemoryToolHandlers.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

using namespace mio;
using nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// fake Embedding：不联网，同一文本 → 同一向量（归一化）
// ---------------------------------------------------------------------------
class FakeEmbedding : public Embedding {
public:
    explicit FakeEmbedding(std::size_t dim = 1024) : dim_(dim) {}
    std::vector<float> embed(const std::string& text) const override {
        std::vector<float> v(dim_, 0.0f);
        for (const unsigned char c : text) {
            std::uint64_t h = 1469598103934665603ULL;
            h ^= c;
            h *= 1099511628211ULL;
            v[h % dim_] += 1.0f;
        }
        double norm = 0.0;
        for (float x : v) norm += static_cast<double>(x) * x;
        if (norm <= 0.0) {
            v[0] = 1.0f;
            return v;
        }
        norm = std::sqrt(norm);
        for (float& x : v) x = static_cast<float>(x / norm);
        return v;
    }

private:
    std::size_t dim_;
};

// ---------------------------------------------------------------------------
// 测试夹具：所有落盘都在临时目录，每个用例一份干净目录
// ---------------------------------------------------------------------------
struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<Achieve> archive;
    std::unique_ptr<MemoryStore> store;
    std::shared_ptr<Embedding> embedding;
    std::unique_ptr<MemoryManager> memory;
    std::unique_ptr<ProposalStore> proposals;
    std::unique_ptr<FactStore> facts;
    std::unique_ptr<CognitionStore> cognitions;
    std::unique_ptr<RelationshipGraph> graph;
    ToolRegistry registry;
    ToolHandlerContext ctx;
    AccessContext access;

    std::string conv = "private:alpha";
    std::string otherConv = "private:beta";

    explicit Fixture(const std::string& name) {
        dir = std::filesystem::temp_directory_path() /
              ("mio_test_d_" + std::to_string(static_cast<long>(::getpid())) + "_" + name);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "archive", ec);

        archive = std::make_unique<Achieve>(dir / "archive");
        store = std::make_unique<MemoryStore>(dir / "memory.db");
        embedding = std::make_shared<FakeEmbedding>(1024);
        memory = std::make_unique<MemoryManager>(MemoryConfig{}, embedding, *store);
        proposals = std::make_unique<ProposalStore>(dir / "proposals.json");
        facts = std::make_unique<FactStore>(dir / "facts.json");
        cognitions = std::make_unique<CognitionStore>(dir / "cognitions.json");
        graph = std::make_unique<RelationshipGraph>(dir / "relationships.json");

        ctx.archive = archive.get();
        ctx.memory = memory.get();
        ctx.proposals = proposals.get();
        ctx.facts = facts.get();
        ctx.cognitions = cognitions.get();
        ctx.graph = graph.get();

        access.conversationKey = conv;
        access.scope = ConversationScope::Private;
        access.requesterPersonId = "u1";
        access.platform = "test";
        access.platformUserId = "1001";
        access.participants = {"u1"};
        access.maxVisibility = Visibility::Conversation;
        access.allowCrossConversation = false;
        access.adminChannel = false;
        access.now = 1700000000;
        ctx.accessProvider = [this]() { return access; };
    }

    // 追加一条消息并返回分配到的 messageId
    std::int64_t append(const std::string& text, Role role = Role::User,
                        const std::string& convKey = "", std::int64_t ts = 0) {
        Msg m;
        m.role = role;
        m.text = text;
        m.createdAt = ts > 0 ? ts : access.now;
        const std::string key = convKey.empty() ? conv : convKey;
        const AppendResult r = archive->appendChecked(key, m);
        return r.messageId;
    }

    void appendRaw(const Msg& m, const std::string& convKey = "") {
        const std::string key = convKey.empty() ? conv : convKey;
        archive->appendChecked(key, m);
    }

    json call(const std::string& tool, const json& args) {
        return json::parse(registry.invoke(tool, args));
    }
    std::string callRaw(const std::string& tool, const json& args) {
        return registry.invoke(tool, args);
    }

    void registerAll() {
        registerMemoryAndConversationTools(registry, ctx);
        registerLegacyCompatTools(registry, ctx);
    }
};

// ---------------------------------------------------------------------------
// envelope 断言
// ---------------------------------------------------------------------------
void checkOk(const json& r) {
    CHECK_TRUE(r.contains("ok"));
    CHECK_TRUE(r.value("ok", false));
    CHECK_TRUE(r.contains("data"));
    CHECK_TRUE(r["data"].is_object());
    CHECK_TRUE(r.contains("error"));
    CHECK_TRUE(r["error"].is_null());
}

void checkErr(const json& r, const std::string& code) {
    CHECK_TRUE(r.contains("ok"));
    CHECK_FALSE(r.value("ok", true));
    CHECK_TRUE(r.contains("data"));
    CHECK_TRUE(r["data"].is_null());
    CHECK_TRUE(r.contains("error"));
    CHECK_TRUE(r["error"].is_object());
    CHECK_EQ(r["error"].value("code", std::string()), code);
    CHECK_FALSE(r["error"].value("message", std::string()).empty());
}

std::string bigText(std::size_t bytes, char fill = 'x') {
    return std::string(bytes, fill);
}

} // namespace

// ===========================================================================
// 1. 注册名 + envelope 形状
// ===========================================================================
MIO_TEST(D_注册名与envelope形状) {
    Fixture fx("reg");
    fx.registerAll();

    const auto names = fx.registry.names();
    auto has = [&](const std::string& n) {
        for (const auto& x : names) {
            if (x == n) return true;
        }
        return false;
    };
    // 逻辑名 → 注册名（下划线形式）
    CHECK_TRUE(has("conversation_list_recent"));
    CHECK_TRUE(has("conversation_search"));
    CHECK_TRUE(has("conversation_get_messages"));
    CHECK_TRUE(has("conversation_get_summary"));
    CHECK_TRUE(has("memory_save_episode"));
    CHECK_TRUE(has("memory_propose_fact"));
    CHECK_TRUE(has("memory_propose_preference"));
    CHECK_TRUE(has("memory_propose_relationship"));
    // 旧别名
    CHECK_TRUE(has("remember"));
    CHECK_TRUE(has("recall_memory"));
    CHECK_TRUE(has("set_nickname"));
    CHECK_TRUE(has("set_notes"));

    // 普通模型上下文不开放任何审批入口，也不注册管理员列表与 set_public
    for (const auto& n : names) {
        CHECK_TRUE(n.find("approve") == std::string::npos);
        CHECK_TRUE(n.find("reject") == std::string::npos);
        CHECK_TRUE(n.find("set_public") == std::string::npos);
        CHECK_TRUE(n.find("memory_list_pending") == std::string::npos);
        CHECK_TRUE(n.find('.') == std::string::npos);  // 点号逻辑名不得直接注册
    }
    CHECK_FALSE(fx.registry.find("memory_list_pending").has_value());

    fx.append("你好");
    // 成功：合法 envelope
    checkOk(fx.call("conversation_list_recent", json::object()));
    // 失败：错误码稳定
    checkErr(fx.call("conversation_list_recent", json{{"limit", "abc"}}),
             "INVALID_ARGUMENT");
    // 参数不是对象
    checkErr(fx.call("conversation_list_recent", json::array()), "INVALID_ARGUMENT");
}

// ===========================================================================
// 2. 读取默认值 / 上限 / 截断标记 / 字节预算
// ===========================================================================
MIO_TEST(D_读取默认值与超限截断) {
    Fixture fx("limits");
    fx.registerAll();
    for (int i = 0; i < 130; ++i) fx.append("消息 " + std::to_string(i));

    const json def = fx.call("conversation_list_recent", json::object());
    checkOk(def);
    CHECK_EQ(def["data"].value("count", 0), 20);           // 默认 20
    CHECK_EQ(def["data"].value("limit", 0), 20);
    CHECK_TRUE(def["data"].value("has_more", false));
    CHECK_TRUE(def["data"].value("truncated", false) == false);  // 未超预算 → 未截断
    CHECK_TRUE(def["data"]["next_cursor"].is_string());

    // limit=1000 → 截断到上限 100 并标记；同时 12000 字节预算独立生效
    const json capped =
        fx.call("conversation_list_recent", json{{"limit", 1000}});
    checkOk(capped);
    CHECK_EQ(capped["data"].value("limit", 0), 100);   // 条数上限 100
    CHECK_TRUE(capped["data"].value("limit_clamped", false));
    CHECK_TRUE(capped["data"].value("count", 0) <= 100);
    CHECK_TRUE(capped["data"].value("has_more", false));
    // 结构化消息本身有 JSON 开销，100 条会先撞上 12000 字节预算 → 必须可见截断
    CHECK_TRUE(capped["data"].value("truncated", false));
    CHECK_TRUE(capped["data"].value("bytes_returned", std::size_t{0}) <=
               limits::kReadMaxBytes);

    // 时间正序（旧 → 新），且"最近"语义保留最新一条
    const auto& msgs = capped["data"]["messages"];
    CHECK_TRUE(!msgs.empty());
    CHECK_TRUE(msgs.front()["message_id"].get<std::int64_t>() <
               msgs.back()["message_id"].get<std::int64_t>());
    CHECK_EQ(msgs.back()["message_id"].get<std::int64_t>(),
             fx.archive->latestMessageId(fx.conv));

    // 游标向后翻页：只返回更旧的，且不重复
    const json page2 = fx.call(
        "conversation_list_recent",
        json{{"limit", 20}, {"cursor", capped["data"]["next_cursor"]}});
    checkOk(page2);
    CHECK_TRUE(page2["data"]["messages"].back()["message_id"].get<std::int64_t>() <
               msgs.front()["message_id"].get<std::int64_t>());

    // 字节预算：30 条 × 2000 字节 → 单次返回必须被截断且 ≤ 12000 字节
    for (int i = 0; i < 30; ++i) fx.append(bigText(2000, static_cast<char>('a' + i % 26)));
    const std::string raw = fx.callRaw(
        "conversation_list_recent", json{{"limit", 100}});
    CHECK_TRUE(raw.size() <= limits::kReadMaxBytes);
    const json big = json::parse(raw);
    checkOk(big);
    CHECK_TRUE(big["data"].value("truncated", false));  // 截断可见
    CHECK_TRUE(big["data"].value("count", 0) < 20);
    CHECK_TRUE(raw.size() <= limits::kReadMaxBytes);
    CHECK_TRUE(big["data"].value("bytes_returned", std::size_t{0}) <=
               limits::kReadMaxBytes);
}

// ===========================================================================
// 3. T09 跨会话读取被拒绝；不泄漏条数/标题/摘要
// ===========================================================================
MIO_TEST(D_跨会话读取被拒绝_T09) {
    Fixture fx("cross");
    fx.registerAll();
    fx.append("ALPHA_SECRET_TEXT");
    fx.append("BETA_SECRET_TEXT", Role::User, fx.otherConv);
    fx.append("BETA_SECRET_SECOND", Role::User, fx.otherConv);

    auto denied = [&](const std::string& tool, json args) {
        args["conversation_key"] = fx.otherConv;
        const std::string raw = fx.callRaw(tool, args);
        checkErr(json::parse(raw), "NOT_FOUND_OR_FORBIDDEN");
        // 不得泄漏其他会话的条数/标题/摘要内容
        CHECK_TRUE(raw.find("BETA_SECRET") == std::string::npos);
        CHECK_TRUE(raw.find("beta") == std::string::npos);
    };
    denied("conversation_list_recent", json::object());
    denied("conversation_search", json{{"query", "SECRET"}});
    denied("conversation_get_messages",
           json{{"from_message_id", 1}, {"to_message_id", 10}});
    denied("conversation_get_summary", json::object());

    // 完全不存在的会话同样统一 NOT_FOUND_OR_FORBIDDEN（不可见与不存在不区分）
    json ghost = json{{"conversation_key", "private:does-not-exist"}};
    checkErr(fx.call("conversation_list_recent", ghost), "NOT_FOUND_OR_FORBIDDEN");

    // 当前会话正常可读
    const json own = fx.call("conversation_list_recent", json::object());
    checkOk(own);
    CHECK_EQ(own["data"].value("count", 0), 1);

    // ---- 游标：篡改 / 跨会话 / 跨查询 ----
    // 只有 1 条时没有下一页
    const json single = fx.call("conversation_list_recent", json{{"limit", 1}});
    checkOk(single);
    CHECK_TRUE(single["data"]["next_cursor"].is_null());

    fx.append("ALPHA_THIRD");
    const json p1 = fx.call("conversation_list_recent", json{{"limit", 1}});
    checkOk(p1);
    CHECK_TRUE(p1["data"]["next_cursor"].is_string());
    const std::string good = p1["data"]["next_cursor"].get<std::string>();

    // 篡改 base64 正文第一个字符（必然改变解码后的 payload）→ INVALID_ARGUMENT
    std::string bad = good;
    const std::size_t body = bad.find('.') + 1;
    CHECK_TRUE(body > 0 && body < bad.size());
    bad[body] = (bad[body] == 'A') ? 'B' : 'A';
    CHECK_NE(bad, good);
    checkErr(fx.call("conversation_list_recent",
                     json{{"limit", 1}, {"cursor", bad}}),
             "INVALID_ARGUMENT");
    // 完全伪造的游标
    checkErr(fx.call("conversation_list_recent",
                     json{{"limit", 1}, {"cursor", "v1.ZmFrZQ==.deadbeef"}}),
             "INVALID_ARGUMENT");
    // 跨查询复用（list_recent 的游标给 search 用）
    checkErr(fx.call("conversation_search", json{{"query", "ALPHA"}, {"cursor", good}}),
             "INVALID_ARGUMENT");
    // 跨会话复用：把 beta 加入允许列表后，用 alpha 的游标读 beta → INVALID_ARGUMENT
    fx.access.allowCrossConversation = true;
    fx.access.allowedConversationKeys = {fx.otherConv};
    checkErr(fx.call("conversation_list_recent",
                     json{{"conversation_key", fx.otherConv},
                          {"limit", 1},
                          {"cursor", good}}),
             "INVALID_ARGUMENT");
    // 授权后跨会话查询本身是允许的（权限判定只看服务端上下文）
    const json allowed =
        fx.call("conversation_list_recent", json{{"conversation_key", fx.otherConv}});
    checkOk(allowed);
    CHECK_EQ(allowed["data"].value("count", 0), 2);
}

// ===========================================================================
// 4. 参数过大 / 缺失的稳定错误码
// ===========================================================================
MIO_TEST(D_参数过大返回稳定错误码) {
    Fixture fx("args");
    fx.registerAll();
    fx.append("seed");

    // query > 1000 字节
    checkErr(fx.call("conversation_search", json{{"query", bigText(1001)}}),
             "LIMIT_EXCEEDED");
    // 1000 字节是允许的上限；无匹配 = 空结果（成功，不是错误）
    const json edge = fx.call("conversation_search", json{{"query", bigText(1000)}});
    checkOk(edge);
    CHECK_EQ(edge["data"].value("count", 1), 0);
    checkErr(fx.call("conversation_search", json{{"query", ""}}), "INVALID_ARGUMENT");
    checkErr(fx.call("conversation_search", json::object()), "INVALID_ARGUMENT");

    // 写入正文 > 4000 字节
    checkErr(fx.call("memory_save_episode", json{{"text", bigText(4001)}}),
             "LIMIT_EXCEEDED");
    checkErr(fx.call("memory_save_episode", json{{"text", ""}}), "INVALID_ARGUMENT");
    checkErr(fx.call("memory_save_episode", json::object()), "INVALID_ARGUMENT");
    // 4000 字节边界必须成功
    checkOk(fx.call("memory_save_episode", json{{"text", bigText(4000)}}));

    // 区间参数
    checkErr(fx.call("conversation_get_messages",
                     json{{"from_message_id", 5}, {"to_message_id", 1}}),
             "INVALID_ARGUMENT");
    checkErr(fx.call("conversation_get_messages",
                     json{{"from_message_id", "x"}, {"to_message_id", 1}}),
             "INVALID_ARGUMENT");
    checkErr(fx.call("conversation_get_messages", json::object()), "INVALID_ARGUMENT");
    checkErr(fx.call("conversation_list_recent", json{{"limit", 0}}),
             "INVALID_ARGUMENT");
    checkErr(fx.call("conversation_list_recent", json{{"limit", -3}}),
             "INVALID_ARGUMENT");
}

// ===========================================================================
// 5. T10 manual 幂等 + 不推进静默游标
// ===========================================================================
MIO_TEST(D_manual幂等与静默游标解耦_T10) {
    Fixture fx("idem");
    fx.registerAll();
    const std::int64_t m1 = fx.append("用户说他喜欢美式咖啡");
    fx.append("记住了");

    CHECK_EQ(fx.memory->committedCursor(fx.conv), static_cast<std::int64_t>(0));
    const std::size_t before = fx.memory->count();

    const json first = fx.call("memory_save_episode",
                               json{{"text", "用户喜欢美式咖啡"},
                                    {"idempotency_key", "k-coffee"},
                                    {"evidence_message_ids",
                                     json::array({fx.conv + "#" + std::to_string(m1)})}});
    checkOk(first);
    CHECK_TRUE(first["data"].value("memory_id", static_cast<std::int64_t>(0)) > 0);
    CHECK_EQ(first["data"].value("status", std::string()), std::string("saved"));
    CHECK_FALSE(first["data"].value("duplicate", true));
    CHECK_TRUE(first["data"].contains("embedding_status"));
    CHECK_EQ(first["data"].value("kind", std::string()), std::string("manual"));
    CHECK_EQ(fx.memory->count(), before + 1);

    const std::int64_t id = first["data"].value("memory_id", static_cast<std::int64_t>(0));

    // 相同 idempotency_key 重复 → 同一 ID + duplicate 状态
    const json again = fx.call("memory_save_episode",
                               json{{"text", "用户喜欢美式咖啡"},
                                    {"idempotency_key", "k-coffee"},
                                    {"evidence_message_ids",
                                     json::array({fx.conv + "#" + std::to_string(m1)})}});
    checkOk(again);
    CHECK_EQ(again["data"].value("memory_id", static_cast<std::int64_t>(0)), id);
    CHECK_EQ(again["data"].value("status", std::string()), std::string("duplicate"));
    CHECK_TRUE(again["data"].value("duplicate", false));
    CHECK_EQ(fx.memory->count(), before + 1);

    // 未给幂等键：服务端用 会话+文本 生成，同文本同样幂等
    const json auto1 = fx.call("memory_save_episode", json{{"text", "自动幂等的文本"}});
    checkOk(auto1);
    const json auto2 = fx.call("memory_save_episode", json{{"text", "自动幂等的文本"}});
    checkOk(auto2);
    CHECK_EQ(auto2["data"].value("memory_id", static_cast<std::int64_t>(0)),
             auto1["data"].value("memory_id", static_cast<std::int64_t>(0)));
    CHECK_EQ(auto2["data"].value("status", std::string()), std::string("duplicate"));
    CHECK_EQ(fx.memory->count(), before + 2);

    // 不同文本 → 新 ID
    const json other = fx.call("memory_save_episode", json{{"text", "另一条完全不同的经历"}});
    checkOk(other);
    CHECK_NE(other["data"].value("memory_id", static_cast<std::int64_t>(0)), id);

    // 关键：manual 记忆不得推进静默总结游标（与静默游标解耦）
    CHECK_EQ(fx.memory->committedCursor(fx.conv), static_cast<std::int64_t>(0));
    const auto stored = fx.memory->getSummary(id);
    CHECK_TRUE(stored.has_value());
    if (stored) {
        CHECK_TRUE(stored->kind == SummaryKind::Manual);
        CHECK_EQ(stored->idempotencyKey, std::string("k-coffee"));
        CHECK_EQ(stored->summary, std::string("用户喜欢美式咖啡"));
    }
}

// ===========================================================================
// 6. T08 提案：不改变事实层、拒绝伪造审核人、请求批准
// ===========================================================================
MIO_TEST(D_提案不改变事实层_T08) {
    Fixture fx("propose");
    fx.registerAll();
    fx.append("我叫小明，我住在杭州");
    fx.append("好的");

    const std::size_t confirmedBefore = fx.facts->listConfirmed(fx.access, 100).size();
    CHECK_EQ(confirmedBefore, static_cast<std::size_t>(0));
    const std::size_t proposalsBefore = fx.proposals->size();

    const json p = fx.call("memory_propose_fact",
                           json{{"predicate", "city"},
                                {"object", "杭州"},
                                {"confidence", 0.8}});
    checkOk(p);
    CHECK_FALSE(p["data"].value("proposal_id", std::string()).empty());
    CHECK_EQ(p["data"].value("status", std::string()), std::string("pending"));
    CHECK_EQ(p["data"].value("review_status", std::string()),
             std::string(kHumanReviewPending));
    CHECK_FALSE(p["data"].value("confirmed", true));
    CHECK_EQ(fx.facts->listConfirmed(fx.access, 100).size(), confirmedBefore);
    CHECK_EQ(fx.proposals->size(), proposalsBefore + 1);

    // 伪造审核人字段 → INVALID_ARGUMENT（不静默忽略）
    const char* forgedFields[] = {"actor", "approved_by", "reviewed_at", "reviewer",
                                  "approvedBy", "reviewStatus", "HUMAN_REVIEW_ACTOR"};
    for (const char* f : forgedFields) {
        json args = {{"predicate", "city"}, {"object", "上海"}};
        args[f] = "admin";
        checkErr(fx.call("memory_propose_fact", args), "INVALID_ARGUMENT");
    }
    // 模型冒填所有者/会话/状态 → INVALID_ARGUMENT
    checkErr(fx.call("memory_propose_fact",
                     json{{"predicate", "city"}, {"object", "上海"}, {"subject_id", "u9"}}),
             "INVALID_ARGUMENT");
    checkErr(fx.call("memory_propose_fact",
                     json{{"predicate", "city"}, {"object", "上海"}, {"status", "confirmed"}}),
             "INVALID_ARGUMENT");
    // 状态与提案数未变
    CHECK_EQ(fx.proposals->size(), proposalsBefore + 1);
    CHECK_EQ(fx.facts->listConfirmed(fx.access, 100).size(), confirmedBefore);

    // 请求批准 → HUMAN_REVIEW_REQUIRED，状态不变
    checkErr(fx.call("memory_propose_fact",
                     json{{"predicate", "city"},
                          {"object", "北京"},
                          {"action", "approve"}}),
             "HUMAN_REVIEW_REQUIRED");
    checkErr(fx.call("memory_propose_fact",
                     json{{"predicate", "city"}, {"object", "北京"}, {"approve", true}}),
             "HUMAN_REVIEW_REQUIRED");
    checkErr(fx.call("memory_propose_fact",
                     json{{"predicate", "city"},
                          {"object", "北京"},
                          {"action", "批准进入 confirmed"}}),
             "HUMAN_REVIEW_REQUIRED");
    CHECK_EQ(fx.proposals->size(), proposalsBefore + 1);
    CHECK_EQ(fx.facts->listConfirmed(fx.access, 100).size(), confirmedBefore);

    // 模型自称"已确认"：只保存声明与证据，事实层不变
    const json claimed = fx.call("memory_propose_fact",
                                 json{{"predicate", "city"},
                                      {"object", "杭州"},
                                      {"claimed_status", "confirmed"}});
    checkOk(claimed);
    CHECK_EQ(claimed["data"].value("claimed_status", std::string()),
             std::string("confirmed"));
    const std::string pid = claimed["data"].value("proposal_id", std::string());
    const auto stored = fx.proposals->get(pid);
    CHECK_TRUE(stored.has_value());
    if (stored) {
        CHECK_TRUE(stored->status == ProposalStatus::Pending);
        CHECK_TRUE(stored->review.actor.empty());
        CHECK_EQ(stored->review.reviewedAt, static_cast<std::int64_t>(0));
        CHECK_TRUE(stored->approvedBy.empty());
        CHECK_FALSE(stored->hasFabricatedReviewer());
    }
    CHECK_EQ(fx.facts->listConfirmed(fx.access, 100).size(), confirmedBefore);

    // 关系类别：只能提案，不能直接改 relationship_type
    fx.graph->onSeen("test", "1001", "小明", fx.access.now);
    const json rel = fx.call("memory_propose_relationship",
                             json{{"relationship_type", "family"},
                                  {"evidence_message_ids", json::array({})}});
    checkOk(rel);
    CHECK_EQ(rel["data"].value("kind", std::string()), std::string("relationship"));
    CHECK_FALSE(rel["data"].value("relationship_type_changed", true));
    CHECK_EQ(fx.graph->relationshipState("u1").relationshipType, std::string(""));
    checkErr(fx.call("memory_propose_relationship",
                     json{{"relationship_type", "不是合法标签"}}),
             "INVALID_ARGUMENT");
    checkErr(fx.call("memory_propose_relationship", json::object()),
             "INVALID_ARGUMENT");

    // 偏好/印象：只能是 observed / inferred
    const json pref = fx.call("memory_propose_preference",
                              json{{"text", "喜欢喝美式咖啡"}, {"kind", "preference"}});
    checkOk(pref);
    CHECK_EQ(pref["data"].value("status", std::string()), std::string("inferred"));
    // inferred 默认不注入稳定 prompt（只有 observed/confirmed 才注入）
    CHECK_FALSE(pref["data"].value("injected_into_default_prompt", true));
    CHECK_FALSE(pref["data"].value("confirmed", true));
    CHECK_FALSE(pref["data"].value("cognition_id", std::string()).empty());
    // 没有证据不能声明 observed
    checkErr(fx.call("memory_propose_preference",
                     json{{"text", "喜欢拿铁"}, {"observed", true}}),
             "INVALID_ARGUMENT");
}

// ===========================================================================
// 7. T08 管理员列表权限
// ===========================================================================
MIO_TEST(D_管理员列表权限_T08) {
    Fixture fx("admin");
    fx.registerAll();
    fx.append("seed");
    checkOk(fx.call("memory_propose_fact",
                    json{{"predicate", "city"}, {"object", "杭州"}}));

    // 模型上下文 adminChannel 恒为 false → 拒绝，且不泄漏提案内容
    const std::string denied = handleListPendingProposals(fx.ctx, json::object());
    checkErr(json::parse(denied), "NOT_FOUND_OR_FORBIDDEN");
    CHECK_TRUE(denied.find("杭州") == std::string::npos);

    // 管理端：可以列出 pending（审核人字段必须为空 / 0）
    fx.access.adminChannel = true;
    const json listed = json::parse(handleListPendingProposals(fx.ctx, json::object()));
    checkOk(listed);
    CHECK_EQ(listed["data"].value("count", 0), 1);
    const auto& item = listed["data"]["proposals"][0];
    CHECK_EQ(item.value("status", std::string()), std::string("pending"));
    CHECK_EQ(item.value("review_status", std::string()), std::string(kHumanReviewPending));
    CHECK_EQ(item.value("review_actor", std::string()), std::string(""));
    CHECK_EQ(item.value("reviewed_at", 1), static_cast<std::int64_t>(0));
    // 管理通道也不提供批准入口
    checkErr(json::parse(handleListPendingProposals(
                 fx.ctx, json{{"action", "approve"}})),
             "HUMAN_REVIEW_REQUIRED");
    checkErr(json::parse(handleListPendingProposals(
                 fx.ctx, json{{"actor", "admin"}})),
             "INVALID_ARGUMENT");
    // 待审阅提案不得进入 listConfirmed
    CHECK_EQ(fx.facts->listConfirmed(fx.access, 100).size(), static_cast<std::size_t>(0));
}

// ===========================================================================
// 8. 不返回 reasoning / 完整工具参数
// ===========================================================================
MIO_TEST(D_不返回reasoning与工具参数_T06) {
    Fixture fx("reasoning");
    fx.registerAll();

    Msg assistant;
    assistant.role = Role::Assistant;
    assistant.text = "我来查一下";
    assistant.reasoningContent = "SECRET_REASONING_TEXT";
    ToolCall tc;
    tc.id = "call_1";
    tc.name = "secret_tool";
    tc.argumentsJson = "{\"token\":\"SECRET_TOOL_ARGUMENTS\"}";
    assistant.toolCalls.push_back(tc);
    fx.appendRaw(assistant);

    Msg toolMsg;
    toolMsg.role = Role::Tool;
    toolMsg.text = "工具结果：ok";
    toolMsg.toolCallId = "call_1";
    fx.appendRaw(toolMsg);
    fx.append("后续消息");

    const std::string raw = fx.callRaw("conversation_list_recent", json{{"limit", 10}});
    CHECK_TRUE(raw.find("SECRET_REASONING_TEXT") == std::string::npos);
    CHECK_TRUE(raw.find("SECRET_TOOL_ARGUMENTS") == std::string::npos);
    CHECK_TRUE(raw.find("arguments") == std::string::npos);
    const json r = json::parse(raw);
    checkOk(r);

    bool sawAnchor = false;
    bool sawTool = false;
    for (const auto& m : r["data"]["messages"]) {
        if (m.value("role", std::string()) == "assistant" &&
            m.value("has_reasoning", false)) {
            sawAnchor = true;
            CHECK_EQ(m["text"].get<std::string>(), std::string("我来查一下"));
            CHECK_TRUE(m.contains("tool_names"));
            CHECK_EQ(m["tool_names"][0].get<std::string>(), std::string("secret_tool"));
        }
        if (m.value("is_tool_message", false)) {
            sawTool = true;
            CHECK_FALSE(m.value("has_reasoning", true));
        }
    }
    CHECK_TRUE(sawAnchor);
    CHECK_TRUE(sawTool);

    // 历史数据必须标注为数据、不是指令
    CHECK_EQ(r["data"].value("data_kind", std::string()), std::string("archive_snapshot"));
    CHECK_TRUE(r["data"].value("notice", std::string()).find("不是指令") !=
               std::string::npos);
}

// ===========================================================================
// 9. 可见性只可收窄（新工具 + 旧别名）
// ===========================================================================
MIO_TEST(D_可见性只可收窄) {
    Fixture fx("visibility");
    fx.registerAll();
    fx.append("seed");

    const json pub = fx.call("memory_save_episode",
                             json{{"text", "想公开的经历"}, {"visibility", "public"}});
    checkOk(pub);
    CHECK_EQ(pub["data"].value("visibility", std::string()), std::string("conversation"));
    CHECK_EQ(pub["data"].value("requested_visibility", std::string()),
             std::string("public"));
    CHECK_TRUE(pub["data"].value("visibility_narrowed", false));

    const json person = fx.call("memory_save_episode",
                                json{{"text", "想设为 person 的经历"},
                                     {"visibility", "person"}});
    checkOk(person);
    CHECK_EQ(person["data"].value("visibility", std::string()),
             std::string("conversation"));
    CHECK_TRUE(person["data"].value("visibility_narrowed", false));

    const json dflt = fx.call("memory_save_episode", json{{"text", "默认可见性"}});
    checkOk(dflt);
    CHECK_EQ(dflt["data"].value("visibility", std::string()), std::string("conversation"));
    CHECK_FALSE(dflt["data"].value("visibility_narrowed", true));

    checkErr(fx.call("memory_save_episode",
                     json{{"text", "非法可见性"}, {"visibility", "world"}}),
             "INVALID_ARGUMENT");

    // 旧别名 remember 的 is_public=true 同样被压回最窄
    const json legacy = fx.call("remember",
                                json{{"text", "旧的公开请求"}, {"is_public", true}});
    checkOk(legacy);
    CHECK_EQ(legacy["data"].value("visibility", std::string()),
             std::string("conversation"));
    CHECK_TRUE(legacy["data"].value("visibility_narrowed", false));
    CHECK_EQ(legacy["data"].value("deprecated_tool", std::string()),
             std::string("remember"));

    // 存到库里的可见性也必须是最窄
    const auto rec = fx.memory->getSummary(
        legacy["data"].value("memory_id", static_cast<std::int64_t>(0)));
    CHECK_TRUE(rec.has_value());
    if (rec) {
        CHECK_TRUE(rec->kind == SummaryKind::Manual);
        CHECK_TRUE(rec->visibility == Visibility::Conversation);
    }
}

// ===========================================================================
// 10. 证据消息 ID 服务端校验
// ===========================================================================
MIO_TEST(D_证据ID服务端校验) {
    Fixture fx("evidence");
    fx.registerAll();
    const std::int64_t id1 = fx.append("证据一");
    const std::int64_t id2 = fx.append("证据二");
    fx.append("BETA 的证据", Role::User, fx.otherConv);
    CHECK_EQ(id1, static_cast<std::int64_t>(1));
    CHECK_EQ(id2, static_cast<std::int64_t>(2));

    // 其他会话的证据（即使该消息真实存在）→ NOT_FOUND_OR_FORBIDDEN
    checkErr(fx.call("memory_save_episode",
                     json{{"text", "跨会话证据"},
                          {"evidence_message_ids",
                           json::array({fx.otherConv + "#1"})}}),
             "NOT_FOUND_OR_FORBIDDEN");
    // 不存在的消息 ID
    checkErr(fx.call("memory_save_episode",
                     json{{"text", "不存在的证据"},
                          {"evidence_message_ids", json::array({9999})}}),
             "NOT_FOUND_OR_FORBIDDEN");
    // 非法格式
    checkErr(fx.call("memory_save_episode",
                     json{{"text", "非法证据"},
                          {"evidence_message_ids", json::array({"abc#x"})}}),
             "INVALID_ARGUMENT");
    checkErr(fx.call("memory_save_episode",
                     json{{"text", "非法证据类型"},
                          {"evidence_message_ids", json{{"a", 1}}}}),
             "INVALID_ARGUMENT");
    // 超过 20 条证据
    json many = json::array();
    for (int i = 0; i < 21; ++i) many.push_back(1);
    checkErr(fx.call("memory_save_episode",
                     json{{"text", "证据过多"}, {"evidence_message_ids", many}}),
             "LIMIT_EXCEEDED");

    // 合法证据：服务端规范化并赋予真实来源与范围
    const json ok = fx.call("memory_save_episode",
                            json{{"text", "带证据的经历"},
                                 {"evidence_message_ids",
                                  json::array({fx.conv + "#2", 1})}});
    checkOk(ok);
    CHECK_EQ(ok["data"]["evidence_message_ids"].size(), static_cast<std::size_t>(2));
    CHECK_EQ(ok["data"]["evidence_message_ids"][0].get<std::string>(),
             fx.conv + "#2");
    CHECK_EQ(ok["data"]["evidence_message_ids"][1].get<std::string>(),
             fx.conv + "#1");
    CHECK_EQ(ok["data"].value("from_message_id", 0), 1);
    CHECK_EQ(ok["data"].value("to_message_id", 0), 2);
    CHECK_FALSE(ok["data"].value("silent_cursor_advanced", true));

    // 无证据时用当前会话已落盘范围（不得凭空写 0 范围）
    const json noEv = fx.call("memory_save_episode", json{{"text", "无证据经历"}});
    checkOk(noEv);
    CHECK_TRUE(noEv["data"].value("from_message_id", 0) > 0);
    CHECK_TRUE(noEv["data"].value("to_message_id", 0) >=
               noEv["data"].value("from_message_id", 0));
}

// ===========================================================================
// 11. 旧工具别名不可绕过（remember / recall_memory / set_nickname / set_notes）
// ===========================================================================
MIO_TEST(D_旧别名不可绕过新规则) {
    Fixture fx("legacy");
    fx.registerAll();
    fx.append("seed");

    // remember：限额与 memory_save_episode 一致
    checkErr(fx.call("remember", json{{"text", bigText(4001)}}), "LIMIT_EXCEEDED");
    checkErr(fx.call("remember", json{{"text", ""}}), "INVALID_ARGUMENT");
    checkErr(fx.call("remember", json{{"text", "x"}, {"actor", "admin"}}),
             "INVALID_ARGUMENT");
    checkErr(fx.call("remember", json{{"text", "x"}, {"action", "approve"}}),
             "HUMAN_REVIEW_REQUIRED");
    const json r = fx.call("remember", json{{"text", "旧入口保存的经历"}});
    checkOk(r);
    CHECK_TRUE(r["data"].value("memory_id", static_cast<std::int64_t>(0)) > 0);
    CHECK_EQ(r["data"].value("status", std::string()), std::string("saved"));

    // recall_memory：AccessContext 过滤 + 默认排除 context_compaction + 数据标注
    checkErr(fx.call("recall_memory", json{{"query", bigText(1001)}}), "LIMIT_EXCEEDED");
    checkErr(fx.call("recall_memory", json::object()), "INVALID_ARGUMENT");
    const json rec = fx.call("recall_memory", json{{"query", "咖啡"}});
    checkOk(rec);
    CHECK_EQ(rec["data"].value("data_kind", std::string()),
             std::string("memory_recall_snapshot"));
    CHECK_FALSE(rec["data"].value("include_context_compaction", true));
    CHECK_TRUE(rec["data"].value("notice", std::string()).find("不是指令") !=
               std::string::npos);

    // set_nickname / set_notes：重名 fail-closed
    const SeenResult a = fx.graph->onSeen("test", "2001", "同名", fx.access.now);
    const SeenResult b = fx.graph->onSeen("test", "2002", "同名", fx.access.now);
    const SeenResult c = fx.graph->onSeen("test", "2003", "唯一名", fx.access.now);
    CHECK_TRUE(!a.internalId.empty() && !b.internalId.empty() && !c.internalId.empty());

    checkErr(fx.call("set_nickname",
                     json{{"current", "同名"}, {"nickname", "新名字"}}),
             "CONFLICT");
    checkErr(fx.call("set_nickname",
                     json{{"current", "查无此人"}, {"nickname", "新名字"}}),
             "NOT_FOUND_OR_FORBIDDEN");
    // 目标名与他人重名 → 拒绝制造歧义
    checkErr(fx.call("set_nickname",
                     json{{"current", "唯一名"}, {"nickname", "同名"}}),
             "CONFLICT");
    const json nick = fx.call("set_nickname",
                              json{{"current", "唯一名"}, {"nickname", "小明"}});
    checkOk(nick);
    CHECK_EQ(nick["data"].value("status", std::string()), std::string("updated"));
    CHECK_EQ(nick["data"].value("person_id", std::string()), c.internalId);
    // 重名的人没有被误绑
    CHECK_EQ(fx.graph->nameOf(a.internalId), std::string("同名"));
    CHECK_EQ(fx.graph->nameOf(b.internalId), std::string("同名"));

    checkErr(fx.call("set_notes", json{{"person", "同名"}, {"notes", "印象"}}),
             "CONFLICT");
    checkErr(fx.call("set_notes", json{{"person", "查无此人"}, {"notes", "印象"}}),
             "NOT_FOUND_OR_FORBIDDEN");
    checkErr(fx.call("set_notes",
                     json{{"person", "小明"}, {"notes", bigText(4001)}}),
             "LIMIT_EXCEEDED");
    checkErr(fx.call("set_notes",
                     json{{"person", "小明"}, {"notes", "印象"}, {"reviewed_at", 1}}),
             "INVALID_ARGUMENT");
    const json notes = fx.call("set_notes",
                               json{{"person", "小明"}, {"notes", "喜欢猫"}});
    checkOk(notes);
    CHECK_EQ(notes["data"].value("person_id", std::string()), c.internalId);

    // set_public 不由 D 注册
    CHECK_FALSE(fx.registry.find("set_public").has_value());
}

// ===========================================================================
// 12. 依赖缺失 / 异常不崩溃（稳定错误码而非抛出）
// ===========================================================================
MIO_TEST(D_依赖缺失返回稳定错误码) {
    Fixture fx("deps");
    fx.registerAll();

    // accessProvider 未接线 → STORAGE_UNAVAILABLE（绝不回退 thread_local）
    ToolHandlerContext bare = fx.ctx;
    bare.accessProvider = nullptr;
    ToolRegistry r2;
    registerMemoryAndConversationTools(r2, bare);
    registerLegacyCompatTools(r2, bare);
    checkErr(json::parse(r2.invoke("conversation_list_recent", json::object())),
             "STORAGE_UNAVAILABLE");
    checkErr(json::parse(r2.invoke("memory_save_episode", json{{"text", "x"}})),
             "STORAGE_UNAVAILABLE");
    checkErr(json::parse(handleListPendingProposals(bare, json::object())),
             "STORAGE_UNAVAILABLE");

    // 档案缺失 → STORAGE_UNAVAILABLE
    ToolHandlerContext noArchive = fx.ctx;
    noArchive.archive = nullptr;
    ToolRegistry r3;
    registerMemoryAndConversationTools(r3, noArchive);
    checkErr(json::parse(r3.invoke("conversation_list_recent", json::object())),
             "STORAGE_UNAVAILABLE");
    checkErr(json::parse(r3.invoke("memory_save_episode", json{{"text", "x"}})),
             "STORAGE_UNAVAILABLE");
    checkErr(json::parse(r3.invoke("conversation_search", json{{"query", "x"}})),
             "STORAGE_UNAVAILABLE");

    // 记忆缺失 → STORAGE_UNAVAILABLE
    ToolHandlerContext noMemory = fx.ctx;
    noMemory.memory = nullptr;
    ToolRegistry r4;
    registerMemoryAndConversationTools(r4, noMemory);
    checkErr(json::parse(r4.invoke("memory_save_episode", json{{"text", "x"}})),
             "STORAGE_UNAVAILABLE");
    checkErr(json::parse(r4.invoke("conversation_get_summary", json::object())),
             "STORAGE_UNAVAILABLE");

    // 提案 / 图谱缺失
    ToolHandlerContext noStores = fx.ctx;
    noStores.proposals = nullptr;
    noStores.cognitions = nullptr;
    noStores.graph = nullptr;
    ToolRegistry r5;
    registerMemoryAndConversationTools(r5, noStores);
    registerLegacyCompatTools(r5, noStores);
    checkErr(json::parse(r5.invoke("memory_propose_fact",
                                   json{{"predicate", "p"}, {"object", "o"}})),
             "STORAGE_UNAVAILABLE");
    checkErr(json::parse(r5.invoke("memory_propose_preference",
                                   json{{"text", "x"}})),
             "STORAGE_UNAVAILABLE");
    checkErr(json::parse(r5.invoke("set_notes",
                                   json{{"person", "谁"}, {"notes", "x"}})),
             "STORAGE_UNAVAILABLE");

    // 没有会话归属 → 写工具 INVALID_ARGUMENT、读工具 NOT_FOUND_OR_FORBIDDEN
    fx.access.conversationKey.clear();
    checkErr(fx.call("memory_save_episode", json{{"text", "x"}}), "INVALID_ARGUMENT");
    checkErr(fx.call("conversation_list_recent", json::object()),
             "NOT_FOUND_OR_FORBIDDEN");
}

// ===========================================================================
// 13. 会话读取：区间读取 / 搜索 / 摘要
// ===========================================================================
MIO_TEST(D_区间搜索与摘要读取) {
    Fixture fx("read");
    fx.registerAll();
    for (int i = 1; i <= 30; ++i) {
        fx.append("消息 " + std::to_string(i) + (i == 7 ? " 关键词NEEDLE" : ""));
    }

    // 区间读取：含端点 + 截断标记
    const json range = fx.call("conversation_get_messages",
                               json{{"from_message_id", 5}, {"to_message_id", 9}});
    checkOk(range);
    CHECK_EQ(range["data"].value("count", 0), 5);
    CHECK_EQ(range["data"]["messages"][0]["message_id"].get<std::int64_t>(), 5);
    CHECK_EQ(range["data"]["messages"][4]["message_id"].get<std::int64_t>(), 9);
    CHECK_EQ(range["data"].value("latest_message_id", 0), 30);

    // 分页：limit=2 → has_more + 游标继续
    const json p1 = fx.call("conversation_get_messages",
                            json{{"from_message_id", 1},
                                 {"to_message_id", 10},
                                 {"limit", 2}});
    checkOk(p1);
    CHECK_EQ(p1["data"].value("count", 0), 2);
    CHECK_TRUE(p1["data"].value("has_more", false));
    CHECK_TRUE(p1["data"]["next_cursor"].is_string());
    const json p2 = fx.call("conversation_get_messages",
                            json{{"from_message_id", 1},
                                 {"to_message_id", 10},
                                 {"limit", 2},
                                 {"cursor", p1["data"]["next_cursor"]}});
    checkOk(p2);
    CHECK_EQ(p2["data"]["messages"][0]["message_id"].get<std::int64_t>(), 3);

    // 搜索：大小写不敏感 + 结果标注
    const json found = fx.call("conversation_search", json{{"query", "needle"}});
    checkOk(found);
    CHECK_EQ(found["data"].value("count", 0), 1);
    CHECK_EQ(found["data"]["messages"][0]["message_id"].get<std::int64_t>(), 7);
    CHECK_EQ(found["data"].value("data_kind", std::string()),
             std::string("archive_snapshot"));

    // 摘要：默认排除 context_compaction
    Msg manualMsg;
    manualMsg.role = Role::User;
    manualMsg.text = "触发摘要";
    (void)manualMsg;
    const json sum = fx.call("conversation_get_summary", json::object());
    checkOk(sum);
    CHECK_EQ(sum["data"].value("data_kind", std::string()),
             std::string("summary_snapshot"));
    CHECK_FALSE(sum["data"].value("include_context_compaction", true));
    for (const auto& s : sum["data"]["summaries"]) {
        CHECK_TRUE(s.value("kind", std::string()) == "episodic_memory" ||
                   s.value("kind", std::string()) == "manual");
    }

    // 未授权会话的摘要同样拒绝
    checkErr(fx.call("conversation_get_summary",
                     json{{"conversation_key", fx.otherConv}}),
             "NOT_FOUND_OR_FORBIDDEN");
}
