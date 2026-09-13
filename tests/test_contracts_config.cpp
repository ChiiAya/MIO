// 共享契约与配置校验用例（主 agent）
// 覆盖：可见性收窄语义 / envelope 形状 / conversation 配置严格校验 /
//       非法热更新完整保留旧状态（T04 前半）

#include "framework.h"

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "core/contracts/Contracts.h"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

using namespace mio;

namespace {

// 严格 UTF-8 校验（截断结果必须仍是完整字符序列）
bool isValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;  // 非法起始字节（含 0x80..0xBF 孤立续字节）
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

struct EnvGuard {
    std::string name;
    bool had = false;
    std::string old;
    EnvGuard(const char* n) : name(n) {
        const char* v = std::getenv(n);
        if (v != nullptr) {
            had = true;
            old = v;
        }
    }
    ~EnvGuard() {
        if (had)
            setenv(name.c_str(), old.c_str(), 1);
        else
            unsetenv(name.c_str());
    }
};

} // namespace

MIO_TEST(可见性_只可收窄) {
    CHECK_EQ(static_cast<int>(narrower(Visibility::Public, Visibility::Conversation)),
             static_cast<int>(Visibility::Conversation));
    CHECK_EQ(static_cast<int>(narrower(Visibility::Person, Visibility::Public)),
             static_cast<int>(Visibility::Person));
    // 摘要输出只可建议收窄：合并结果永远不超过原范围
    CHECK_EQ(static_cast<int>(narrower(Visibility::Conversation, Visibility::Public)),
             static_cast<int>(Visibility::Conversation));
}

MIO_TEST(可见性_未知值保持最窄) {
    Visibility v = Visibility::Public;
    CHECK_FALSE(parseVisibility("secret", v));
    CHECK_TRUE(parseVisibility("person", v));
    CHECK_EQ(std::string(toString(v)), std::string("person"));
    // 解析失败必须是 conversation（最窄），不得默认放宽
    CHECK_EQ(std::string(toString(parseVisibilityOrNarrowest("bogus"))),
             std::string("conversation"));
}

MIO_TEST(envelope_形状稳定) {
    ToolResult ok = ToolResult::success(nlohmann::json{{"id", 7}});
    auto j = nlohmann::json::parse(ok.toJsonString());
    CHECK_TRUE(j["ok"].get<bool>());
    CHECK_EQ(j["data"]["id"].get<int>(), 7);
    CHECK_TRUE(j["error"].is_null());

    ToolResult bad = ToolResult::failure(ErrorCode::NotFoundOrForbidden, "隐藏记录");
    auto j2 = nlohmann::json::parse(bad.toJsonString());
    CHECK_FALSE(j2["ok"].get<bool>());
    CHECK_EQ(std::string(j2["error"]["code"].get<std::string>()),
             std::string("NOT_FOUND_OR_FORBIDDEN"));
    CHECK_TRUE(j2["data"].is_null());
}

MIO_TEST(审阅占位_不得伪造审核人) {
    ReviewPlaceholder p;
    p.status = kHumanReviewPending;
    CHECK_FALSE(p.actorIsFabricated());
    p.actor = "model";
    CHECK_TRUE(p.actorIsFabricated());
    p.actor.clear();
    p.reviewedAt = 12345;
    CHECK_TRUE(p.actorIsFabricated());
    CHECK_EQ(std::string(toString(ErrorCode::HumanReviewRequired)),
             std::string("HUMAN_REVIEW_REQUIRED"));
}

MIO_TEST(会话配置_缺省与合法解析) {
    ConversationLifecycleConfig cfg;
    CHECK_EQ(cfg.silenceTimeoutSeconds, 120);
    CHECK_EQ(cfg.minMessagesBeforeSummary, 2);
    CHECK_EQ(cfg.maxPendingSummaryRetries, 3);
    CHECK_TRUE(cfg.summarizeOnSilence);

    auto j = nlohmann::json::parse(R"({"silenceTimeoutSeconds": 30})");
    auto parsed = conversationLifecycleFromJson(j);
    CHECK_EQ(parsed.silenceTimeoutSeconds, 30);
    // 缺失字段保持默认值（不得被清空）
    CHECK_EQ(parsed.minMessagesBeforeSummary, 2);
    CHECK_EQ(parsed.maxPendingSummaryRetries, 3);
}

MIO_TEST(会话配置_类型错误与越界必须拒绝) {
    bool threw = false;
    try {
        conversationLifecycleFromJson(nlohmann::json::parse(R"({"silenceTimeoutSeconds": "30"})"));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    threw = false;
    try {
        conversationLifecycleFromJson(nlohmann::json::parse(R"({"silenceTimeoutSeconds": 0})"));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    threw = false;
    try {
        conversationLifecycleFromJson(nlohmann::json::parse(R"({"maxPendingSummaryRetries": 11})"));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    threw = false;
    try {
        conversationLifecycleFromJson(nlohmann::json::parse(R"({"minMessagesBeforeSummary": -1})"));
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);
}

MIO_TEST(冷启动预算_类型与范围校验) {
    ContextBuilderConfig c;
    CHECK_EQ(c.coldStartRawTokens, 2000);
    CHECK_EQ(c.coldStartMaxMessages, 20);

    bool threw = false;
    try {
        from_json(nlohmann::json::parse(R"({"coldStartRawTokens": "100"})"), c);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    ContextBuilderConfig c2;
    threw = false;
    try {
        from_json(nlohmann::json::parse(R"({"coldStartMaxMessages": -3})"), c2);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);

    // 0 合法：表示不恢复历史原文
    ContextBuilderConfig c3;
    from_json(nlohmann::json::parse(R"({"coldStartRawTokens": 0, "coldStartMaxMessages": 0})"), c3);
    CHECK_EQ(c3.coldStartRawTokens, 0);
    CHECK_EQ(c3.coldStartMaxMessages, 0);
}

MIO_TEST(非法热更新_完整保留旧配置) {
    auto initial = std::make_shared<const AppConfig>(AppConfig{});
    ConfigManager mgr(initial);

    // 先做一次合法更新
    CHECK_TRUE(mgr.reload(R"({"conversation": {"silenceTimeoutSeconds": 45},
                              "botName": "Mio2"})"));
    CHECK_EQ(mgr.get()->conversation.silenceTimeoutSeconds, 45);
    CHECK_EQ(mgr.get()->botName, std::string("Mio2"));

    // 非法更新：越界 → 整次失败，且 botName 也不得被部分套用
    CHECK_FALSE(mgr.reload(R"({"conversation": {"silenceTimeoutSeconds": 999999},
                               "botName": "HACKED"})"));
    CHECK_EQ(mgr.get()->conversation.silenceTimeoutSeconds, 45);
    CHECK_EQ(mgr.get()->botName, std::string("Mio2"));

    // 非法类型
    CHECK_FALSE(mgr.reload(R"({"conversation": {"summarizeOnSilence": "yes-please"}})"));
    CHECK_TRUE(mgr.get()->conversation.summarizeOnSilence);
    CHECK_EQ(mgr.get()->conversation.silenceTimeoutSeconds, 45);

    // 局部更新：未出现的字段保持原值
    CHECK_TRUE(mgr.reload(R"({"conversation": {"minMessagesBeforeSummary": 9}})"));
    CHECK_EQ(mgr.get()->conversation.minMessagesBeforeSummary, 9);
    CHECK_EQ(mgr.get()->conversation.silenceTimeoutSeconds, 45);
}

MIO_TEST(环境变量_只覆盖存在的字段) {
    EnvGuard g1("MIO_SILENCE_TIMEOUT_SECONDS");
    EnvGuard g2("MIO_MIN_MESSAGES_BEFORE_SUMMARY");
    EnvGuard g3("MIO_SUMMARIZE_ON_SILENCE");

    unsetenv("MIO_SILENCE_TIMEOUT_SECONDS");
    unsetenv("MIO_MIN_MESSAGES_BEFORE_SUMMARY");
    unsetenv("MIO_SUMMARIZE_ON_SILENCE");

    AppConfig cfg;
    cfg.conversation.minMessagesBeforeSummary = 7;
    cfg.conversation.summarizeOnSilence = false;
    setenv("MIO_SILENCE_TIMEOUT_SECONDS", "11", 1);
    std::string err;
    CHECK_TRUE(cfg.applyEnvironment(&err));
    CHECK_EQ(cfg.conversation.silenceTimeoutSeconds, 11);
    // 未设置的环境变量不得重置既有字段
    CHECK_EQ(cfg.conversation.minMessagesBeforeSummary, 7);
    CHECK_FALSE(cfg.conversation.summarizeOnSilence);

    // 非法环境变量：失败且不部分套用
    setenv("MIO_SILENCE_TIMEOUT_SECONDS", "0", 1);
    CHECK_FALSE(cfg.applyEnvironment(&err));
    CHECK_FALSE(err.empty());
}

MIO_TEST(摘要记录_强制字段校验) {
    SummaryRecord rec;
    CHECK_FALSE(validateSummaryRecord(rec).ok);  // 缺 conversation_key

    rec.conversationKey = "private:1";
    rec.fromMessageId = 3;
    rec.toMessageId = 5;
    rec.source = "silence_summary";
    rec.summary = "用户说喜欢喝茶";
    CHECK_TRUE(validateSummaryRecord(rec).ok);

    rec.toMessageId = 2;  // 范围倒挂
    CHECK_FALSE(validateSummaryRecord(rec).ok);

    rec.toMessageId = 5;
    rec.source.clear();
    CHECK_FALSE(validateSummaryRecord(rec).ok);
}

MIO_TEST(摘要类型_未知值拒绝写入) {
    SummaryKind k = SummaryKind::ContextCompaction;
    CHECK_FALSE(parseSummaryKind("episodic", k));
    CHECK_TRUE(parseSummaryKind("episodic_memory", k));
    CHECK_EQ(std::string(toString(k)), std::string("episodic_memory"));
    // context_compaction 默认不得进入长期召回
    CHECK_FALSE(recallableByDefault(SummaryKind::ContextCompaction));
    CHECK_TRUE(recallableByDefault(SummaryKind::EpisodicMemory));
    CHECK_FALSE(recallableByDefault(SummaryKind::Manual));
}

MIO_TEST(示例配置_无凭据样例可加载) {
    // config.example.json 必须能被真实解析（示例配置是新字段的交付物之一）
    std::ifstream in("config.example.json");
    if (!in) {
        // 从 build 输出目录运行时回退到项目根
        in.open("../../config.example.json");
    }
    CHECK_TRUE(static_cast<bool>(in));
    std::stringstream ss;
    ss << in.rdbuf();

    AppConfig cfg;
    from_json(nlohmann::json::parse(ss.str()), cfg);
    std::string err;
    CHECK_TRUE(cfg.validate(&err));
    CHECK_EQ(cfg.conversation.silenceTimeoutSeconds, 120);
    CHECK_EQ(cfg.conversation.minMessagesBeforeSummary, 2);
    CHECK_EQ(cfg.conversation.maxPendingSummaryRetries, 3);
    CHECK_TRUE(cfg.conversation.summarizeOnSilence);
    CHECK_EQ(cfg.contextBuilder.coldStartRawTokens, 2000);
    CHECK_EQ(cfg.contextBuilder.coldStartMaxMessages, 20);
    // 样例不得包含真实凭据
    CHECK_EQ(cfg.openai.apiKey, std::string("REPLACE_ME"));
    CHECK_EQ(cfg.embedding.apiKey, std::string("REPLACE_ME"));
    CHECK_EQ(cfg.napcat.token, std::string("REPLACE_ME"));
}

MIO_TEST(UTF8截断_不产生半个字符) {
    const std::string zh = "你好世界";  // 每个汉字 3 字节
    bool truncated = false;
    const std::string cut = limits::truncateUtf8(zh, 4, &truncated);
    CHECK_TRUE(truncated);
    CHECK_EQ(cut, std::string("你"));  // 4 字节会切碎第二个汉字 → 回退到 3 字节
    truncated = false;
    CHECK_EQ(limits::truncateUtf8(zh, 6, &truncated), std::string("你好"));
    CHECK_TRUE(truncated);
    truncated = false;
    CHECK_EQ(limits::truncateUtf8(zh, 12, &truncated), zh);
    CHECK_FALSE(truncated);
    // 截断结果必须是合法 UTF-8（完整字符序列）：不能出现孤立续字节
    const std::string mixed = "a你b好c世d界";
    for (std::size_t n = 0; n <= mixed.size(); ++n) {
        const std::string s = limits::truncateUtf8(mixed, n);
        CHECK_TRUE(s.size() <= n);
        CHECK_TRUE(isValidUtf8(s));
        CHECK_TRUE(mixed.rfind(s, 0) == 0);  // 必须是原串前缀（不跳字节）
    }
    CHECK_TRUE(isValidUtf8(limits::truncateUtf8("😀😀😀", 5)));  // 4 字节 emoji
}

MIO_TEST(记忆后端_类型错误拒绝_未知名称交给工厂降级) {
    MemoryConfig c;
    CHECK_EQ(c.backend, std::string("sqlite-local"));

    bool threw = false;
    try {
        MemoryConfig c2;
        from_json(nlohmann::json::parse(R"({"backend": 123})"), c2);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);  // 类型错误 → 整次重载失败

    // 未知名称不是配置非法：由工厂降级为 unavailable 并记 WARN
    MemoryConfig c3;
    from_json(nlohmann::json::parse(R"({"backend": "no-such-backend"})"), c3);
    CHECK_EQ(c3.backend, std::string("no-such-backend"));

    // 写入可见性上限不得放宽为 public
    threw = false;
    try {
        MemoryConfig c4;
        from_json(nlohmann::json::parse(R"({"writeVisibilityCap": "public"})"), c4);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);
}

MIO_TEST(访问上下文_默认仅当前会话) {
    AccessContext ctx;
    ctx.conversationKey = "private:1";
    CHECK_TRUE(ctx.canAccessConversation("private:1"));
    CHECK_FALSE(ctx.canAccessConversation("private:2"));
    // 即使显式打开跨会话，白名单为空时也不得放行
    ctx.allowCrossConversation = true;
    CHECK_FALSE(ctx.canAccessConversation("private:2"));
    ctx.allowedConversationKeys.push_back("private:2");
    CHECK_TRUE(ctx.canAccessConversation("private:2"));
    // 可见性过滤：默认上限为 conversation
    CHECK_TRUE(ctx.canSee(Visibility::Conversation));
    CHECK_FALSE(ctx.canSee(Visibility::Public));
}
