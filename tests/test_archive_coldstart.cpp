// 子任务 F 验收用例：档案稳定 ID / 冷启动（无 LLM）/ 压缩全覆盖 / 投影不变量
//
// 覆盖文档验收项：
//   T06 冷启动、重新冷启动、历史检索：不含 reasoning；冷启动不调摘要 LLM；
//       不残留孤立工具消息；
//   T07 超长消息与超长历史：双重预算有效、截断可见、中间范围不静默丢失。
// 约束：所有档案写到临时目录，绝不读写真实 data/；摘要用 fake Llm（不联网）。

#include "framework.h"

#include "context/achieve/Achieve.h"
#include "context/achieve/ColdStart.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "context/conversationFusion/FusionContext.h"
#include "context/conversationFusion/FusionRouter.h"
#include "context/costEstimator/Tokens.h"
#include "context/summarizor/SummaryManager.h"
#include "core/contracts/Contracts.h"
#include "core/event/Event.h"
#include "core/message/Message.h"
#include "mind/graph/RelationshipGraph.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace mio;

namespace {

// ---------------------------------------------------------------------------
// 临时目录：测试绝不触碰真实 data/
// ---------------------------------------------------------------------------
std::atomic<int>& tempCounter() {
    static std::atomic<int> c{0};
    return c;
}

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& tag) {
        path = std::filesystem::temp_directory_path() /
               ("mio_f_" + tag + "_" + std::to_string(::getpid()) + "_" +
                std::to_string(++tempCounter()));
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// fake Llm：统计调用次数并记录请求，用于证明"冷启动没有调用摘要模型"
class FakeLlm : public Llm {
public:
    std::string reply = "压缩后的摘要正文\n话题：测试话题\n私密性判断：私密";
    std::atomic<int> calls{0};

    ChatResponse chat(const ChatRequest& req) override {
        ++calls;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            requests_.push_back(req);
        }
        ChatResponse r;
        r.text = reply;
        return r;
    }

    int callCount() const { return calls.load(); }

    std::vector<ChatRequest> requests() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return requests_;
    }

    // 所有请求材料拼在一起（覆盖性断言用）
    std::string allMaterial() const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::string all;
        for (const auto& q : requests_)
            for (const auto& m : q.messages) all += m.text;
        return all;
    }

private:
    mutable std::mutex mtx_;
    std::vector<ChatRequest> requests_;
};

ConversationKey privKey(const std::string& id, const std::string& platform = "console") {
    return ConversationKey{ConversationScope::Private, id, platform};
}

ConversationKey groupKey(const std::string& id, const std::string& platform = "qq") {
    return ConversationKey{ConversationScope::Group, id, platform};
}

Msg mk(Role role, const std::string& text, std::int64_t createdAt = 0) {
    Msg m;
    m.role = role;
    m.text = text;
    m.createdAt = createdAt;
    return m;
}

// 旧格式档案行（无 messageId）
std::string legacyLine(const std::string& role, const std::string& text,
                       std::int64_t createdAt) {
    return "{\"role\":\"" + role + "\",\"text\":\"" + text +
           "\",\"createdAt\":" + std::to_string(createdAt) + "}";
}

std::vector<std::int64_t> idsOf(const std::vector<ArchiveRecord>& recs) {
    std::vector<std::int64_t> ids;
    ids.reserve(recs.size());
    for (const auto& r : recs) ids.push_back(r.messageId);
    return ids;
}

// 重复一段 UTF-8 文本（不能用 std::string(n, '啊')：多字节字面量会被截成单字节）
std::string repeatUtf8(const std::string& unit, int times) {
    std::string out;
    out.reserve(unit.size() * static_cast<std::size_t>(times));
    for (int i = 0; i < times; ++i) out += unit;
    return out;
}

// 目录快照：文件名 → 文件字节（只读性断言：枚举前后必须完全一致）
std::map<std::string, std::string> snapshotDir(const std::filesystem::path& dir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        out[entry.path().filename().string()] = ss.str();
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// F1：Msg 与持久化兼容
// ---------------------------------------------------------------------------
MIO_TEST(F_Msg_messageId持久化与旧记录兼容) {
    // 旧记录没有 messageId → 0（正常解析，不报错）
    const auto old = nlohmann::json::parse(R"({"role":"user","text":"旧消息"})");
    const Msg legacy = msgFromJson(old);
    CHECK_EQ(legacy.messageId, std::int64_t{0});
    CHECK_EQ(legacy.text, std::string("旧消息"));

    // 新记录往返
    Msg m{Role::Assistant};
    m.text = "新消息";
    m.messageId = 42;
    m.reasoningContent = "推理内容";  // 协议需要：reasoning 仍随消息持久化
    m.truncated = true;               // 投影标记：不落盘
    const auto j = msgToJson(m);
    CHECK_EQ(j.value("messageId", std::int64_t{0}), std::int64_t{42});
    CHECK_FALSE(j.contains("truncated"));
    const Msg back = msgFromJson(j);
    CHECK_EQ(back.messageId, std::int64_t{42});
    CHECK_EQ(back.reasoningContent, std::string("推理内容"));
    CHECK_FALSE(back.truncated);

    // 0 = 未分配：不写字段（旧行为不变）
    const Msg zero{Role::User};
    CHECK_FALSE(msgToJson(zero).contains("messageId"));
}

// ---------------------------------------------------------------------------
// F2：稳定消息 ID、区间读取、IArchiveReader
// ---------------------------------------------------------------------------
MIO_TEST(F_稳定消息ID与区间读取) {
    TempDir tmp("ids");
    Achieve achieve(tmp.path / "history");
    const ConversationKey key = privKey("u1");

    for (int i = 1; i <= 3; ++i) {
        Msg m = mk(Role::User, "m" + std::to_string(i), 1000 + i);
        const AppendResult r = achieve.appendChecked(key, m);
        CHECK_TRUE(r.ok);
        CHECK_EQ(r.messageId, std::int64_t{i});
    }
    CHECK_EQ(achieve.latestMessageId(key), std::int64_t{3});
    CHECK_EQ(achieve.latestMessageId(key.toString()), std::int64_t{3});

    const auto range = achieve.readRange(key.toString(), 1, 2, 0);
    CHECK_EQ(range.size(), std::size_t{2});
    CHECK_EQ(range[0].messageId, std::int64_t{1});
    CHECK_EQ(range[1].messageId, std::int64_t{2});
    CHECK_EQ(achieve.readRange(key, 2, 2, 0).size(), std::size_t{1});
    CHECK_EQ(achieve.readRange(key, 1, 2, 1).size(), std::size_t{1});  // limit 生效

    // 最近 N 条：时间正序返回
    const auto recent = achieve.readRecent(key, 2);
    CHECK_EQ(recent.size(), std::size_t{2});
    CHECK_EQ(recent[0].text, std::string("m2"));
    CHECK_EQ(recent[1].text, std::string("m3"));

    // 重新打开（走文件解析 + 旁路索引）ID 不变
    Achieve reopened(tmp.path / "history");
    CHECK_EQ(reopened.latestMessageId(key), std::int64_t{3});
    const auto again = reopened.readRange(key, 0, 0, 0);
    CHECK_TRUE(idsOf(again) == std::vector<std::int64_t>({1, 2, 3}));

    // 通过 IArchiveReader 只读契约访问（B/D 消费路径）
    IArchiveReader& reader = achieve;
    CHECK_EQ(reader.latestMessageId(key.toString()), std::int64_t{3});
    CHECK_EQ(reader.readRange(key.toString(), 1, 3, 0).size(), std::size_t{3});
    CHECK_EQ(reader.readRecent(key.toString(), 1).size(), std::size_t{1});
    CHECK_EQ(reader.search(key.toString(), "m2", 0).size(), std::size_t{1});
    CHECK_TRUE(reader.damageReport().empty());
}

MIO_TEST(F_旧档案迁移_索引可重建且ID稳定) {
    TempDir tmp("legacy");
    const auto dir = tmp.path / "history";
    std::filesystem::create_directories(dir);
    const ConversationKey key = privKey("u2");
    const auto legacyFile = dir / "private_u2.jsonl";  // 平台前缀引入前的旧命名

    std::vector<std::string> originalLines;
    {
        std::ofstream out(legacyFile);
        for (int i = 1; i <= 5; ++i) {
            const std::string line = legacyLine("user", "L" + std::to_string(i), 1000 + i);
            originalLines.push_back(line);
            out << line << "\n";
        }
    }

    // 读到旧命名文件就用它（不改名、不搬家）
    CHECK_EQ(Achieve::resolvePathFor(dir, key.toString()).string(), legacyFile.string());

    std::vector<std::int64_t> ids1;
    {
        Achieve a(dir);
        ids1 = idsOf(a.readRange(key, 0, 0, 0));  // 触发加载（损坏/映射在此生成）
        CHECK_EQ(a.latestMessageId(key), std::int64_t{5});
    }
    CHECK_EQ(ids1.size(), std::size_t{5});
    for (std::size_t i = 0; i < ids1.size(); ++i)
        CHECK_EQ(ids1[i], static_cast<std::int64_t>(i + 1));

    // 旁路索引已生成，且原档案未被批量改写（旧行保持原样）
    const auto idx = dir / "private_u2.jsonl.idx.json";
    CHECK_TRUE(std::filesystem::exists(idx));
    {
        std::ifstream in(legacyFile);
        std::string line;
        std::size_t n = 0;
        while (std::getline(in, line)) {
            CHECK_EQ(line, originalLines[n]);
            ++n;
        }
        CHECK_EQ(n, std::size_t{5});
    }

    // 删除索引后重建：ID 完全一致（ID 只由文件内容确定性推导）
    std::filesystem::remove(idx);
    {
        Achieve a2(dir);
        const auto ids2 = idsOf(a2.readRange(key, 0, 0, 0));
        CHECK_TRUE(ids2 == ids1);
    }
    CHECK_TRUE(std::filesystem::exists(idx));  // 重建

    // 再次加载：ID 不变
    {
        Achieve a3(dir);
        CHECK_TRUE(idsOf(a3.readRange(key, 0, 0, 0)) == ids1);
    }

    // 追加新记录：新行带 messageId，旧行保持无 ID（不批量改写唯一原档案）
    {
        Achieve a4(dir);
        const AppendResult r = a4.appendChecked(key, mk(Role::User, "L6", 1006));
        CHECK_TRUE(r.ok);
        CHECK_EQ(r.messageId, std::int64_t{6});
    }
    {
        std::ifstream in(legacyFile);
        std::string line;
        std::size_t n = 0;
        bool lastHasId = false;
        while (std::getline(in, line)) {
            ++n;
            if (n <= 5) CHECK_EQ(line, originalLines[n - 1]);
            if (n == 6) lastHasId = line.find("\"messageId\":6") != std::string::npos;
        }
        CHECK_EQ(n, std::size_t{6});
        CHECK_TRUE(lastHasId);
    }
    // 混排（前 5 行无 ID、第 6 行显式 ID）→ 再加载 ID 仍是 1..6
    {
        Achieve a5(dir);
        CHECK_TRUE(idsOf(a5.readRange(key, 0, 0, 0)) ==
                   std::vector<std::int64_t>({1, 2, 3, 4, 5, 6}));
        CHECK_EQ(a5.latestMessageId(key), std::int64_t{6});
    }
}

MIO_TEST(F_混排显式ID与按位置补) {
    TempDir tmp("mixed");
    const auto dir = tmp.path / "history";
    std::filesystem::create_directories(dir);
    const ConversationKey key = groupKey("10001", "qq");
    const auto file = dir / "group_10001.jsonl";  // 旧命名（无平台前缀）

    {
        std::ofstream out(file);
        out << legacyLine("user", "a", 1) << "\n";                       // 位置 1
        out << legacyLine("assistant", "b", 2) << "\n";                  // 位置 2
        out << legacyLine("user", "c", 3) << "\n";                       // 位置 3
        out << "{\"role\":\"assistant\",\"text\":\"d\",\"messageId\":50}\n";  // 显式 50
        out << legacyLine("user", "e", 5) << "\n";                       // 显式之后 → 51
        out << legacyLine("assistant", "f", 6) << "\n";                  // 52
    }
    Achieve a(dir);
    const auto ids = idsOf(a.readRange(key, 0, 0, 0));
    CHECK_TRUE(ids == std::vector<std::int64_t>({1, 2, 3, 50, 51, 52}));
    CHECK_EQ(a.latestMessageId(key), std::int64_t{52});

    // 重建（新实例 + 删索引）结果一致
    std::filesystem::remove(dir / "group_10001.jsonl.idx.json");
    Achieve a2(dir);
    CHECK_TRUE(idsOf(a2.readRange(key, 0, 0, 0)) ==
               std::vector<std::int64_t>({1, 2, 3, 50, 51, 52}));
    // 新追加顺延最大 ID
    CHECK_EQ(a2.appendChecked(key, mk(Role::User, "g")).messageId, std::int64_t{53});
}

MIO_TEST(F_路径编码防会话键碰撞) {
    TempDir tmp("paths");
    const auto dir = tmp.path / "history";
    std::filesystem::create_directories(dir);

    const std::vector<ConversationKey> keys = {
        privKey("a/b"),
        privKey("a_b"),      // 旧消毒会与 a/b 撞名
        privKey(".."),
        privKey("a..b"),
        privKey("带 空格 的", "qq"),
        privKey("用户😀", "qq"),
        groupKey(std::string(400, 'x')),
        groupKey(std::string(399, 'x') + "y"),
    };
    std::vector<std::string> paths;
    for (const auto& k : keys)
        paths.push_back(Achieve::primaryPathFor(dir, k).string());
    for (std::size_t i = 0; i < paths.size(); ++i) {
        for (std::size_t j = i + 1; j < paths.size(); ++j)
            CHECK_NE(paths[i], paths[j]);
        // 文件名长度有界（超长 id 不得撑爆文件系统上限）
        CHECK_TRUE(std::filesystem::path(paths[i]).filename().string().size() <= 255);
    }

    // 实际写入互不覆盖
    Achieve achieve(dir);
    CHECK_TRUE(achieve.appendChecked(keys[0], mk(Role::User, "first")).ok);
    CHECK_TRUE(achieve.appendChecked(keys[1], mk(Role::User, "second")).ok);
    const auto r0 = achieve.readRecent(keys[0], 0);
    const auto r1 = achieve.readRecent(keys[1], 0);
    CHECK_EQ(r0.size(), std::size_t{1});
    CHECK_EQ(r1.size(), std::size_t{1});
    CHECK_EQ(r0[0].text, std::string("first"));
    CHECK_EQ(r1[0].text, std::string("second"));
    CHECK_NE(paths[0], paths[1]);
}

MIO_TEST(F_损坏行报告位置且后续消息可读) {
    TempDir tmp("damage");
    const auto dir = tmp.path / "history";
    std::filesystem::create_directories(dir);
    const ConversationKey key = privKey("u3");
    const auto file = dir / "private_u3.jsonl";  // 旧命名兼容读取
    {
        std::ofstream out(file);
        out << legacyLine("user", "m1", 1) << "\n";
        out << legacyLine("assistant", "m2", 2) << "\n";
        out << legacyLine("user", "m3", 3) << "\n";
        out << "{\"role\":\"user\",\"text\":\"BROKEN\"\n";  // 第 4 行：非法 JSON
        out << legacyLine("user", "m4", 5) << "\n";
        out << legacyLine("assistant", "m5", 6) << "\n";
    }

    Achieve achieve(dir);
    // 先读触发加载（damageReport 报告的是已加载会话的损坏情况）
    const auto recs = achieve.readRange(key, 0, 0, 0);
    CHECK_EQ(recs.size(), std::size_t{5});

    // 损坏行位置 + 原因可观察，且不把整段历史当成不存在
    const auto dmg = achieve.damageReport();
    CHECK_EQ(dmg.size(), std::size_t{1});
    CHECK_EQ(dmg[0].lineNumber, std::int64_t{4});
    CHECK_FALSE(dmg[0].reason.empty());
    CHECK_TRUE(dmg[0].filePath.find("private_u3.jsonl") != std::string::npos);

    // 损坏行占一个 ID 位置（后续 ID 不塌陷）：1,2,3,[4],5,6
    CHECK_EQ(achieve.latestMessageId(key), std::int64_t{6});
    CHECK_TRUE(idsOf(recs) == std::vector<std::int64_t>({1, 2, 3, 5, 6}));
    bool foundM4 = false;
    for (const auto& r : recs) {
        if (r.text != "m4") continue;
        foundM4 = true;
        CHECK_EQ(r.messageId, std::int64_t{5});  // 损坏行之后的消息仍正常读出
    }
    CHECK_TRUE(foundM4);
}

MIO_TEST(F_整个文件打不开时可观察) {
    if (::geteuid() == 0) return;  // root 绕过权限位，跳过该断言
    TempDir tmp("unreadable");
    const auto dir = tmp.path / "history";
    std::filesystem::create_directories(dir);
    const ConversationKey key = privKey("u4");
    const auto file = Achieve::primaryPathFor(dir, key.toString());
    {
        std::ofstream out(file);
        out << legacyLine("user", "hidden", 1) << "\n";
    }
    std::filesystem::permissions(file, std::filesystem::perms::none);

    Achieve achieve(dir);
    CHECK_EQ(achieve.latestMessageId(key), std::int64_t{0});
    const auto dmg = achieve.damageReport();
    CHECK_FALSE(dmg.empty());  // 不是"没有历史"，而是可观察的损坏
    CHECK_EQ(dmg[0].lineNumber, std::int64_t{0});
    CHECK_FALSE(dmg[0].reason.empty());

    std::filesystem::permissions(file, std::filesystem::perms::owner_all);
}

MIO_TEST(F_写失败可观察且缓存不前进) {
    TempDir tmp("writefail");
    const auto dir = tmp.path / "history";
    Achieve achieve(dir);
    const ConversationKey key = privKey("u9");
    const auto path = Achieve::primaryPathFor(dir, key.toString());
    std::error_code ec;
    // 用同名目录占位：ofstream 追加必然失败（即使 root 也不会成功）
    std::filesystem::create_directories(path, ec);
    CHECK_TRUE(std::filesystem::is_directory(path));

    const Msg m = mk(Role::User, "写不进去");
    const AppendResult r = achieve.appendChecked(key, m);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK_EQ(r.messageId, std::int64_t{0});
    // 缓存与最大 ID 不得前进（未确认落盘的消息不进入总结范围）
    CHECK_EQ(achieve.latestMessageId(key), std::int64_t{0});
    CHECK_EQ(achieve.count(key), std::size_t{0});
    CHECK_EQ(achieve.pendingRange(key, 0).count, std::int64_t{0});
    CHECK_TRUE(achieve.coldStart(key, ColdStartOptions{2000, 20, {}}).msgs.empty());

    // 兼容包装不得静默成功：void 接口通过异常让失败可观察
    bool threw = false;
    try {
        achieve.append(key, m);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK_TRUE(threw);
    CHECK_EQ(achieve.latestMessageId(key), std::int64_t{0});

    // 移除障碍后恢复正常
    std::filesystem::remove_all(path, ec);
    const AppendResult ok = achieve.appendChecked(key, m);
    CHECK_TRUE(ok.ok);
    CHECK_EQ(ok.messageId, std::int64_t{1});
    CHECK_EQ(achieve.latestMessageId(key), std::int64_t{1});
}

// ---------------------------------------------------------------------------
// F3：冷启动（无 LLM）
// ---------------------------------------------------------------------------
MIO_TEST(F_冷启动投影剔除reasoning与工具回合_T06) {
    TempDir tmp("cold");
    Achieve achieve(tmp.path / "history");
    const ConversationKey key = privKey("u1");

    Msg u1 = mk(Role::User, "u1", 1);
    Msg a1 = mk(Role::Assistant, "a1", 2);
    a1.reasoningContent = "REASON_SECRET_TEXT";
    Msg anchor = mk(Role::Assistant, "", 3);  // 只有 toolCalls
    anchor.toolCalls.push_back(ToolCall{"c1", "secret_tool", "{\"k\":\"v\"}"});
    anchor.reasoningContent = "REASON_SECRET_TOOL";
    Msg toolResult = mk(Role::Tool, "工具返回正文", 4);
    toolResult.toolCallId = "c1";
    Msg orphanTool = mk(Role::Tool, "孤立工具结果", 5);
    orphanTool.toolCallId = "c_missing";
    Msg u2 = mk(Role::User, "u2", 6);
    Msg a2 = mk(Role::Assistant, "a2", 7);

    for (const Msg& m : {u1, a1, anchor, toolResult, orphanTool, u2, a2})
        CHECK_TRUE(achieve.appendChecked(key, m).ok);

    // 冷启动：只有用户/助手可见正文；reasoning 与工具回合整体排除
    ColdStartOptions opts;
    opts.rawTokenBudget = 2000;
    opts.maxMessages = 20;
    const ColdStartResult cs = achieve.coldStart(key, opts);
    CHECK_EQ(cs.msgs.size(), std::size_t{4});
    CHECK_EQ(cs.msgs[0].text, std::string("u1"));
    CHECK_EQ(cs.msgs[1].text, std::string("a1"));
    CHECK_EQ(cs.msgs[2].text, std::string("u2"));
    CHECK_EQ(cs.msgs[3].text, std::string("a2"));
    CHECK_TRUE(cs.droppedToolRounds >= 3);  // 锚点 + 成对结果 + 孤立结果
    for (const auto& m : cs.msgs) {
        CHECK_TRUE(m.reasoningContent.empty());
        CHECK_TRUE(m.toolCalls.empty());
        CHECK_FALSE(m.role == Role::Tool);
    }
    CHECK_TRUE(cs.msgs.front().messageId < cs.msgs.back().messageId);  // 时间正序

    // 历史检索投影：reasoning 只以 hasReasoning 可观察，绝不返回内容
    const auto recs = achieve.readRange(key.toString(), 0, 0, 0);
    bool sawA1 = false, sawAnchor = false, sawTool = false, sawOrphan = false;
    for (const auto& r : recs) {
        CHECK_TRUE(r.text.find("REASON_SECRET") == std::string::npos);
        if (r.messageId == 2) {
            sawA1 = true;
            CHECK_TRUE(r.hasReasoning);
            CHECK_EQ(r.text, std::string("a1"));
        }
        if (r.messageId == 3) {
            sawAnchor = true;
            CHECK_TRUE(r.hasReasoning);
            CHECK_EQ(r.toolNames.size(), std::size_t{1});
            CHECK_EQ(r.toolNames[0], std::string("secret_tool"));
            CHECK_TRUE(r.text.find("argumentsJson") == std::string::npos);
            CHECK_TRUE(r.text.find("{\"k\":\"v\"}") == std::string::npos);
        }
        if (r.messageId == 4) {
            sawTool = true;
            CHECK_TRUE(r.isToolMessage);
        }
        if (r.messageId == 5) sawOrphan = true;  // 孤立 tool 消息必须被剔除
    }
    CHECK_TRUE(sawA1);
    CHECK_TRUE(sawAnchor);   // 成对闭合的工具回合保留（锚点 + 结果）
    CHECK_TRUE(sawTool);
    CHECK_FALSE(sawOrphan);

    // 未闭合的锚点（有调用无结果）也必须整体剔除
    {
        TempDir tmp2("cold2");
        Achieve a2store(tmp2.path / "history");
        const ConversationKey k2 = privKey("u2");
        Msg open = mk(Role::Assistant, "open", 1);
        open.toolCalls.push_back(ToolCall{"x1", "t", "{}"});
        CHECK_TRUE(a2store.appendChecked(k2, open).ok);
        CHECK_TRUE(a2store.appendChecked(k2, mk(Role::User, "later", 2)).ok);
        const auto rs = a2store.readRange(k2, 0, 0, 0);
        for (const auto& r : rs) CHECK_FALSE(r.messageId == 1);
        CHECK_EQ(a2store.coldStart(k2, ColdStartOptions{2000, 20, {}}).msgs.size(),
                 std::size_t{1});
    }
}

MIO_TEST(F_冷启动双重预算与UTF8截断_T07) {
    TempDir tmp("budget");
    Achieve achieve(tmp.path / "history");
    const ConversationKey key = privKey("u6");
    for (int i = 1; i <= 30; ++i)
        CHECK_TRUE(achieve.appendChecked(key, mk(Role::User, "m" + std::to_string(i),
                                                 1000 + i))
                       .ok);

    ColdStartOptions opts;
    opts.rawTokenBudget = 2000;
    opts.maxMessages = 5;
    const ColdStartResult cs = achieve.coldStart(key, opts);
    CHECK_EQ(cs.msgs.size(), std::size_t{5});          // 只恢复最近 N 条
    CHECK_EQ(cs.msgs.front().text, std::string("m26"));
    CHECK_EQ(cs.msgs.back().text, std::string("m30"));
    CHECK_EQ(cs.fromMessageId, std::int64_t{26});
    CHECK_EQ(cs.toMessageId, std::int64_t{30});
    for (std::size_t i = 1; i < cs.msgs.size(); ++i)
        CHECK_TRUE(cs.msgs[i - 1].messageId < cs.msgs[i].messageId);  // 时间正序
    CHECK_TRUE(cs.droppedByBudget > 0);  // 未恢复的条数可观察，不静默

    // 0 = 不恢复历史原文（两个预算任一为 0）
    CHECK_TRUE(achieve.coldStart(key, ColdStartOptions{0, 20, {}}).msgs.empty());
    CHECK_TRUE(achieve.coldStart(key, ColdStartOptions{2000, 0, {}}).msgs.empty());

    // 超长单条正文：UTF-8 安全截断且可观察
    Msg longMsg = mk(Role::User, repeatUtf8("啊", 3000), 2000);
    CHECK_TRUE(achieve.appendChecked(key, longMsg).ok);
    ColdStartOptions tiny;
    tiny.rawTokenBudget = 100;
    tiny.maxMessages = 5;
    const ColdStartResult cs2 = achieve.coldStart(key, tiny);
    CHECK_EQ(cs2.msgs.size(), std::size_t{1});
    CHECK_TRUE(cs2.anyTruncated);
    CHECK_TRUE(cs2.msgs[0].truncated);
    CHECK_FALSE(cs2.msgs[0].text.empty());
    CHECK_TRUE(estimateTokens(cs2.msgs[0].text) <= 100);      // 不超预算
    CHECK_EQ(cs2.msgs[0].text.size() % 3, std::size_t{0});    // '啊' 3 字节：没切碎

    // 当前批次不重复加入（按 messageId 排除）
    ColdStartOptions ex;
    ex.rawTokenBudget = 2000;
    ex.maxMessages = 3;
    ex.excludeMessageIds.push_back(31);  // 最新一条（超长消息）
    const ColdStartResult cs3 = achieve.coldStart(key, ex);
    CHECK_EQ(cs3.msgs.size(), std::size_t{3});
    CHECK_EQ(cs3.msgs.back().text, std::string("m30"));
}

MIO_TEST(F_ContextBuilder冷启动预算与当前回合优先) {
    TempDir tmp("builder");
    Achieve achieve(tmp.path / "history");
    const ConversationKey key = privKey("u7");
    for (int i = 1; i <= 30; ++i)
        CHECK_TRUE(achieve.appendChecked(key, mk(Role::User, "h" + std::to_string(i),
                                                 1000 + i))
                       .ok);

    auto llm = std::make_shared<FakeLlm>();
    auto summary = std::make_shared<SummaryManager>(800, llm);

    ContextBuilderConfig cfg;
    cfg.budgetTokens = 16000;  // 不触发压缩，只验证冷启动
    cfg.coldStartRawTokens = 2000;
    cfg.coldStartMaxMessages = 3;
    ContextBuilder builder(cfg, summary);
    builder.setColdStartSource(&achieve);

    Msg current = mk(Role::User, "当前回合", 3000);
    FusionUnit unit(key, {current});
    unit.setTopic("已有话题");  // 关掉 topic 预热提醒，断言更干净

    BuildInput in;
    in.systemPrompt = "系统提示";
    in.unit = &unit;
    in.now = 3000;
    const BuildResult r = builder.build(in);
    CHECK_TRUE(r.coldStarted);
    CHECK_EQ(r.coldStartRecovered, 3);
    CHECK_EQ(llm->callCount(), 0);  // 冷启动不调用摘要模型
    CHECK_EQ(r.request.messages.size(), std::size_t{4});  // 3 条历史 + 当前回合
    CHECK_EQ(r.request.messages[0].text, std::string("h28"));
    CHECK_EQ(r.request.messages[2].text, std::string("h30"));
    CHECK_EQ(r.request.messages[3].text, std::string("当前回合"));  // 不重复加入
    CHECK_TRUE(r.request.messages[0].messageId > 0);

    // 已冷启动的 unit 不会被重复恢复（历史已进入 unit 上下文，不再重复膨胀）
    const BuildResult again = builder.build(in);
    CHECK_FALSE(again.coldStarted);
    CHECK_EQ(again.request.messages.size(), std::size_t{4});
    CHECK_EQ(again.request.messages[0].text, std::string("h28"));

    // 预算 0 = 不恢复任何历史原文
    ContextBuilderConfig cfg0 = cfg;
    cfg0.coldStartRawTokens = 0;
    ContextBuilder builder0(cfg0, summary);
    builder0.setColdStartSource(&achieve);
    FusionUnit unit0(key, {current});
    unit0.setTopic("已有话题");
    BuildInput in0 = in;
    in0.unit = &unit0;
    const BuildResult r0 = builder0.build(in0);
    CHECK_FALSE(r0.coldStarted);
    CHECK_EQ(r0.request.messages.size(), std::size_t{1});
    CHECK_EQ(r0.request.messages[0].text, std::string("当前回合"));

    // 条数预算 0 = 同样不恢复
    ContextBuilderConfig cfg1 = cfg;
    cfg1.coldStartMaxMessages = 0;
    ContextBuilder builder1(cfg1, summary);
    builder1.setColdStartSource(&achieve);
    FusionUnit unit1(key, {current});
    unit1.setTopic("已有话题");
    BuildInput in1 = in;
    in1.unit = &unit1;
    CHECK_FALSE(builder1.build(in1).coldStarted);

    // Runtime 顺序（先落档案、再 build）：当前回合已在档案里但 unit 里没有 messageId，
    // 兜底冷启动不得把它重复加入
    {
        TempDir tmp2("builder2");
        Achieve achieve2(tmp2.path / "history");
        for (int i = 1; i <= 5; ++i)
            CHECK_TRUE(achieve2.appendChecked(key, mk(Role::User, "p" + std::to_string(i),
                                                     4000 + i))
                           .ok);
        Msg cur = mk(Role::User, "当前回合", 4100);
        CHECK_TRUE(achieve2.appendChecked(key, cur).ok);
        ContextBuilderConfig cfg2 = cfg;
        cfg2.coldStartMaxMessages = 10;
        ContextBuilder builder2(cfg2, summary);
        builder2.setColdStartSource(&achieve2);
        FusionUnit unit2(key, {cur});
        unit2.setTopic("已有话题");
        BuildInput in2;
        in2.systemPrompt = "系统提示";
        in2.unit = &unit2;
        in2.now = 4100;
        const BuildResult r2 = builder2.build(in2);
        CHECK_TRUE(r2.coldStarted);
        int sameText = 0;
        for (const auto& m : r2.request.messages)
            if (m.text == "当前回合") ++sameText;
        CHECK_EQ(sameText, 1);  // 只出现一次
        CHECK_EQ(r2.request.messages.back().text, std::string("当前回合"));
    }
}

MIO_TEST(F_路由冷启动不调摘要LLM_T06) {
    TempDir tmp("router");
    Achieve achieve(tmp.path / "history");
    const std::int64_t now = 1'700'000'000;
    const ConversationKey key = privKey("u1");

    Msg u1 = mk(Role::User, "你好", now - 100);
    u1.senderId = "u1";
    u1.platform = "console";
    Msg a1 = mk(Role::Assistant, "你好呀", now - 90);
    a1.reasoningContent = "REASON_SECRET";
    Msg u2 = mk(Role::User, "今天天气不错", now - 80);
    u2.senderId = "u1";
    u2.platform = "console";
    CHECK_TRUE(achieve.appendChecked(key, u1).ok);
    CHECK_TRUE(achieve.appendChecked(key, a1).ok);
    CHECK_TRUE(achieve.appendChecked(key, u2).ok);

    auto llm = std::make_shared<FakeLlm>();
    auto summary = std::make_shared<SummaryManager>(800, llm);
    RelationshipGraph graph(tmp.path / "relationships.json");
    graph.ensureMio("MIO");
    graph.onSeen("console", "u1", "u1", now);
    FusionRouter router(graph, achieve, summary, FusionConfig{}, nullptr);

    IncomingMessage in;
    in.conversation = key;
    in.senderId = "u1";
    in.senderName = "u1";
    in.platform = "console";
    in.text = "新消息";
    FusionUnit* unit = router.route(in, now);
    CHECK_TRUE(unit != nullptr);
    CHECK_EQ(llm->callCount(), 0);  // 冷启动没有调用摘要模型
    CHECK_TRUE(unit->coldStartDone());
    CHECK_FALSE(unit->isPublic());       // 默认私密（不再由摘要判定放宽）
    CHECK_TRUE(unit->topic().empty());   // 无 LLM → 无话题
    CHECK_EQ(unit->context().size(), std::size_t{3});
    for (const auto& m : unit->context()) CHECK_TRUE(m.reasoningContent.empty());
    CHECK_TRUE(unit->context()[0].messageId > 0);  // 恢复的消息带稳定 ID

    // 同来源再次路由：复用 unit，不重复冷启动
    FusionUnit* unit2 = router.route(in, now + 1);
    CHECK_TRUE(unit2 == unit);
    CHECK_EQ(unit->context().size(), std::size_t{3});
    CHECK_EQ(llm->callCount(), 0);
}

// ---------------------------------------------------------------------------
// F4：上下文压缩（context_compaction，全覆盖，不写经历/不提升可见性）
// ---------------------------------------------------------------------------
MIO_TEST(F_压缩分块覆盖全部目标范围_T07) {
    TempDir tmp("compact");
    auto llm = std::make_shared<FakeLlm>();
    llm->reply = "第N块摘要正文\n话题：压缩测试\n私密性判断：公开";
    SummaryManager summary(800, llm);

    std::vector<SummaryKind> kinds;
    bool sawProposals = false;
    summary.setOnSummary([&](const SummaryOutcome& o) {
        kinds.push_back(o.kind);
        if (!o.factProposals.empty() || !o.relationshipProposals.empty())
            sawProposals = true;
    });

    std::vector<Msg> ctx;
    for (int i = 1; i <= 50; ++i) {
        Msg m = mk(i % 2 == 1 ? Role::User : Role::Assistant,
                   "m" + std::to_string(i) + ":" + repeatUtf8("好", 60), 1000 + i);
        m.messageId = i;
        if (i == 25) m.reasoningContent = "REASON_SECRET_25";
        if (i == 26) m.reasoningContent = "REASON_SECRET_26";
        ctx.push_back(m);
    }
    const ConversationKey key = privKey("u1");
    FusionUnit unit(key, ctx);

    CompressOptions opts;
    opts.rawKeep = 5;
    opts.chunkTokens = 600;  // 小预算 → 强制多块
    opts.maxChunks = 64;
    const CompressResult r = unit.reColdStart(summary, opts);

    CHECK_TRUE(r.fullCoverage);
    CHECK_TRUE(r.uncoveredRanges.empty());
    CHECK_TRUE(r.chunks >= 2);
    CHECK_EQ(r.failedChunks, 0);
    CHECK_EQ(static_cast<int>(kinds.size()), r.chunks);
    for (auto k : kinds) CHECK_EQ(static_cast<int>(k), static_cast<int>(SummaryKind::ContextCompaction));
    CHECK_FALSE(sawProposals);  // 压缩路径不产出事实/关系提案

    // 目标范围 1..45 每一条都必须被某个分块覆盖（中间范围不得静默丢失）
    const std::string all = llm->allMaterial();
    for (int i = 1; i <= 45; ++i)
        CHECK_TRUE(all.find("m" + std::to_string(i) + ":") != std::string::npos);
    CHECK_TRUE(all.find("m46:") == std::string::npos);  // 尾部原文不重复摘要
    CHECK_TRUE(all.find("REASON_SECRET_25") == std::string::npos);  // reasoning 不入材料
    CHECK_TRUE(all.find("REASON_SECRET_26") == std::string::npos);

    // 结果 = 合并摘要（含全部块）+ 最近 5 条原文
    CHECK_EQ(unit.context().size(), std::size_t{6});
    const std::string& mergedText = unit.context().front().text;
    CHECK_TRUE(mergedText.find("[压缩 1/") != std::string::npos);
    std::size_t markers = 0;  // 每个分块都必须出现在合并摘要里（块数一致）
    for (std::size_t pos = mergedText.find("[压缩 ");
         pos != std::string::npos; pos = mergedText.find("[压缩 ", pos + 1))
        ++markers;
    CHECK_EQ(markers, static_cast<std::size_t>(r.chunks));
    CHECK_EQ(unit.context()[1].text.find("m46:"), std::size_t{0});
    CHECK_EQ(unit.context().back().text.find("m50:"), std::size_t{0});

    // 分块上限：被丢弃的范围必须显式标记（不静默）
    FusionUnit unit2(key, ctx);
    CompressOptions capped = opts;
    capped.maxChunks = 1;
    const CompressResult r2 = unit2.reColdStart(summary, capped);
    CHECK_FALSE(r2.fullCoverage);
    CHECK_FALSE(r2.uncoveredRanges.empty());
    CHECK_TRUE(unit2.context().front().text.find("未覆盖") != std::string::npos);
}

MIO_TEST(F_压缩可见性只可收窄_不产生副作用) {
    TempDir tmp("visibility");
    const ConversationKey key = privKey("u1");
    std::vector<Msg> ctx;
    for (int i = 1; i <= 12; ++i)
        ctx.push_back(mk(Role::User, "c" + std::to_string(i), 1000 + i));

    // 私密 unit + 摘要判定"公开" → 保持私密（不得放宽）
    {
        auto llm = std::make_shared<FakeLlm>();
        llm->reply = "摘要\n话题：t\n私密性判断：公开";
        SummaryManager summary(800, llm);
        FusionUnit unit(key, ctx);
        unit.setIsPublic(false);
        const CompressResult r = unit.reColdStart(summary);
        CHECK_TRUE(r.chunks >= 1);
        CHECK_FALSE(unit.isPublic());
    }
    // 公开 unit + 摘要判定"私密" → 收窄为私密（允许）
    {
        auto llm = std::make_shared<FakeLlm>();
        llm->reply = "摘要\n话题：t\n私密性判断：私密";
        SummaryManager summary(800, llm);
        FusionUnit unit(key, ctx);
        unit.setIsPublic(true);
        unit.reColdStart(summary);
        CHECK_FALSE(unit.isPublic());
    }
    // 重新冷启动不调用"经历记忆"路径：只发 ContextCompaction 的 sink 通知
    {
        auto llm = std::make_shared<FakeLlm>();
        SummaryManager summary(800, llm);
        std::vector<SummaryKind> kinds;
        summary.setOnSummary(
            [&](const SummaryOutcome& o) { kinds.push_back(o.kind); });
        FusionUnit unit(key, ctx);
        unit.reColdStart(summary);
        CHECK_FALSE(kinds.empty());
        for (auto k : kinds)
            CHECK_EQ(static_cast<int>(k),
                     static_cast<int>(SummaryKind::ContextCompaction));
    }
}

// ---------------------------------------------------------------------------
// F5：pendingRange
// ---------------------------------------------------------------------------
MIO_TEST(F_pendingRange只统计用户与可见助手) {
    TempDir tmp("pending");
    Achieve achieve(tmp.path / "history");
    const ConversationKey key = privKey("u5");

    CHECK_EQ(achieve.appendChecked(key, mk(Role::User, "u1")).messageId, std::int64_t{1});

    Msg anchor = mk(Role::Assistant, "");
    anchor.toolCalls.push_back(ToolCall{"c1", "t", "{}"});
    CHECK_EQ(achieve.appendChecked(key, anchor).messageId, std::int64_t{2});

    Msg tool = mk(Role::Tool, "tool out");
    tool.toolCallId = "c1";
    CHECK_EQ(achieve.appendChecked(key, tool).messageId, std::int64_t{3});

    CHECK_EQ(achieve.appendChecked(key, mk(Role::Assistant, "a1")).messageId,
             std::int64_t{4});
    CHECK_EQ(achieve.appendChecked(key, mk(Role::User, "[框架]本会话还没有话题记录"))
                 .messageId,
             std::int64_t{5});
    CHECK_EQ(achieve.appendChecked(key, mk(Role::User, "u2")).messageId, std::int64_t{6});
    CHECK_EQ(achieve.appendChecked(key, mk(Role::System, "sys")).messageId,
             std::int64_t{7});
    Msg a2 = mk(Role::Assistant, "a2");
    a2.reasoningContent = "reasoning";
    CHECK_EQ(achieve.appendChecked(key, a2).messageId, std::int64_t{8});

    const Achieve::PendingRange all = achieve.pendingRange(key, 0);
    CHECK_EQ(all.fromMessageId, std::int64_t{1});
    CHECK_EQ(all.toMessageId, std::int64_t{8});
    CHECK_EQ(all.count, std::int64_t{4});  // 1,4,6,8：不计工具/框架提醒/系统/reasoning

    const Achieve::PendingRange tail = achieve.pendingRange(key, 4);
    CHECK_EQ(tail.fromMessageId, std::int64_t{6});
    CHECK_EQ(tail.toMessageId, std::int64_t{8});
    CHECK_EQ(tail.count, std::int64_t{2});

    const Achieve::PendingRange none = achieve.pendingRange(key, 8);
    CHECK_EQ(none.count, std::int64_t{0});
    CHECK_EQ(none.fromMessageId, std::int64_t{0});
    CHECK_EQ(none.toMessageId, std::int64_t{0});

    // 只统计已确认落盘的消息：写失败后计数不变
    const ConversationKey bad = privKey("u5b");
    const auto badPath = Achieve::primaryPathFor(tmp.path / "history", bad.toString());
    std::error_code ec;
    std::filesystem::create_directories(badPath, ec);
    CHECK_FALSE(achieve.appendChecked(bad, mk(Role::User, "failed")).ok);
    const Achieve::PendingRange badRange = achieve.pendingRange(bad, 0);
    CHECK_EQ(badRange.count, std::int64_t{0});
    CHECK_EQ(badRange.toMessageId, std::int64_t{0});
}

// ---------------------------------------------------------------------------
// 检索
// ---------------------------------------------------------------------------
MIO_TEST(F_search大小写不敏感且不含reasoning) {
    TempDir tmp("search");
    Achieve achieve(tmp.path / "history");
    const ConversationKey key = privKey("u8");

    CHECK_TRUE(achieve.appendChecked(key, mk(Role::User, "Hello World")).ok);
    Msg a1 = mk(Role::Assistant, "普通正文");
    a1.reasoningContent = "REASONING-SECRET-ONLY";
    CHECK_TRUE(achieve.appendChecked(key, a1).ok);
    CHECK_TRUE(achieve.appendChecked(key, mk(Role::User, "今天天气不错")).ok);
    Msg tool = mk(Role::Tool, "hello from tool");
    tool.toolCallId = "c1";
    CHECK_TRUE(achieve.appendChecked(key, tool).ok);

    const auto lower = achieve.search(key, "hello", 0);
    CHECK_EQ(lower.size(), std::size_t{1});  // 工具消息不返回
    CHECK_EQ(lower[0].messageId, std::int64_t{1});
    CHECK_EQ(achieve.search(key, "HELLO", 0).size(), std::size_t{1});  // 大小写不敏感
    CHECK_EQ(achieve.search(key, "reasoning-secret-only", 0).size(),
             std::size_t{0});  // reasoning 不参与检索
    const auto zh = achieve.search(key, "天气", 0);
    CHECK_EQ(zh.size(), std::size_t{1});
    CHECK_EQ(zh[0].messageId, std::int64_t{3});
    CHECK_EQ(achieve.search(key, "o", 1).size(), std::size_t{1});  // limit 生效
    CHECK_EQ(achieve.search(key, "", 0).size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 启动枚举：conversationKeys()（旁路注册表 + 目录扫描，只读）
// ---------------------------------------------------------------------------
MIO_TEST(F_conversationKeys枚举已有会话且排序稳定) {
    TempDir tmp("enum_basic");
    const auto dir = tmp.path / "history";
    Achieve achieve(dir);
    const ConversationKey k2 = privKey("u2");
    const ConversationKey k1 = privKey("u1");
    const ConversationKey kg = groupKey("10001");
    // 乱序写入，枚举必须按 key 排序
    CHECK_TRUE(achieve.appendChecked(k2, mk(Role::User, "b")).ok);
    CHECK_TRUE(achieve.appendChecked(kg, mk(Role::User, "g")).ok);
    CHECK_TRUE(achieve.appendChecked(k1, mk(Role::User, "a")).ok);

    CHECK_TRUE(std::filesystem::exists(dir / Achieve::registryFileName()));
    const auto keys1 = achieve.conversationKeys();
    CHECK_EQ(keys1.size(), std::size_t{3});
    CHECK_TRUE(std::is_sorted(keys1.begin(), keys1.end()));
    CHECK_TRUE(std::find(keys1.begin(), keys1.end(), k1.toString()) != keys1.end());
    CHECK_TRUE(std::find(keys1.begin(), keys1.end(), k2.toString()) != keys1.end());
    CHECK_TRUE(std::find(keys1.begin(), keys1.end(), kg.toString()) != keys1.end());
    // 重复调用结果一致
    CHECK_TRUE(achieve.conversationKeys() == keys1);

    // 新实例（注册表在）同样能列出
    Achieve other(dir);
    CHECK_TRUE(other.conversationKeys() == keys1);
}

MIO_TEST(F_conversationKeys注册表删除后可由目录扫描重建) {
    TempDir tmp("enum_rebuild");
    const auto dir = tmp.path / "history";
    const ConversationKey k1 = privKey("u1");
    const ConversationKey k2 = privKey("u2");
    std::vector<std::string> before;
    {
        Achieve achieve(dir);
        CHECK_TRUE(achieve.appendChecked(k1, mk(Role::User, "a")).ok);
        CHECK_TRUE(achieve.appendChecked(k2, mk(Role::User, "b")).ok);
        before = achieve.conversationKeys();
        CHECK_EQ(before.size(), std::size_t{2});
    }
    // 删除注册表：新命名的目录扫描必须能无损反解（往返校验）
    std::filesystem::remove(dir / Achieve::registryFileName());
    {
        Achieve rebuilt(dir);
        const auto keys = rebuilt.conversationKeys();
        CHECK_TRUE(keys == before);
        // 纯枚举不得创建注册表（只读：不创建任何文件）
        CHECK_FALSE(std::filesystem::exists(dir / Achieve::registryFileName()));
    }
    // 再次构造（注册表仍不存在）结果不变
    Achieve again(dir);
    CHECK_TRUE(again.conversationKeys() == before);
    CHECK_FALSE(std::filesystem::exists(dir / Achieve::registryFileName()));
}

MIO_TEST(F_conversationKeys纯枚举不改目录字节) {
    TempDir tmp("enum_readonly");
    const auto dir = tmp.path / "history";
    {
        Achieve achieve(dir);
        CHECK_TRUE(achieve.appendChecked(privKey("u1"), mk(Role::User, "a")).ok);
    }
    // 快照包含 jsonl + idx + 注册表；枚举前后必须逐字节一致
    const auto before = snapshotDir(dir);
    CHECK_FALSE(before.empty());
    Achieve fresh(dir);
    const auto keys1 = fresh.conversationKeys();
    const auto keys2 = fresh.conversationKeys();
    CHECK_TRUE(keys1 == keys2);
    CHECK_TRUE(snapshotDir(dir) == before);

    // 空目录：枚举不得创建任何文件（注册表也不例外）
    TempDir tmp2("enum_empty");
    const auto dir2 = tmp2.path / "history";
    std::filesystem::create_directories(dir2);
    Achieve empty(dir2);
    CHECK_TRUE(empty.conversationKeys().empty());
    CHECK_TRUE(snapshotDir(dir2).empty());
    CHECK_FALSE(std::filesystem::exists(dir2 / Achieve::registryFileName()));
}

MIO_TEST(F_conversationKeys忽略索引与临时文件并诊断旧命名) {
    TempDir tmp("enum_scan");
    const auto dir = tmp.path / "history";
    const ConversationKey key = privKey("u1");
    {
        Achieve achieve(dir);
        CHECK_TRUE(achieve.appendChecked(key, mk(Role::User, "x")).ok);
    }
    // 旧命名档案（平台前缀丢失，无法无损反解真实 key）
    {
        std::ofstream out(dir / "private_0d00.jsonl");
        out << legacyLine("user", "old", 1) << "\n";
    }
    // 临时文件与"注册表 tmp"都不是档案
    {
        std::ofstream out(dir / "_conversations.json.tmp");
        out << "{}";
    }
    {
        std::ofstream out(dir / "leftover.jsonl.tmp");
        out << "{}";
    }
    // 删掉注册表 → 只能靠目录扫描
    std::filesystem::remove(dir / Achieve::registryFileName());

    Achieve achieve(dir);
    const auto keys = achieve.conversationKeys();
    CHECK_EQ(keys.size(), std::size_t{1});
    CHECK_EQ(keys[0], key.toString());
    // 不把 .idx.json / 临时文件当档案（key 里不会出现这些名字）
    for (const auto& k : keys) {
        CHECK_TRUE(k.find("idx") == std::string::npos);
        CHECK_TRUE(k.find("tmp") == std::string::npos);
    }
    // 旧命名：跳过并留诊断（不猜 key、不抛异常、不影响其它会话）
    const auto diag = achieve.discoveryDiagnostics();
    bool sawLegacy = false;
    for (const auto& d : diag) {
        CHECK_FALSE(d.reason.empty());
        if (d.fileName == "private_0d00.jsonl") sawLegacy = true;
    }
    CHECK_TRUE(sawLegacy);
    // 枚举本身仍然成功（异常安全）
    CHECK_TRUE(achieve.conversationKeys() == keys);
}

MIO_TEST(F_conversationKeys旧命名会话经注册表可枚举) {
    TempDir tmp("enum_legacy");
    const auto dir = tmp.path / "history";
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "private_0d00.jsonl");
        out << legacyLine("user", "old", 1) << "\n";
    }
    const ConversationKey key = privKey("0d00");  // consoleprivate:0d00 → 旧文件
    CHECK_EQ(Achieve::resolvePathFor(dir, key.toString()).filename().string(),
             std::string("private_0d00.jsonl"));

    // 收到该会话消息 → 解析路径 + 登记注册表（自然重建）
    {
        Achieve achieve(dir);
        CHECK_TRUE(achieve.appendChecked(key, mk(Role::User, "new", 2)).ok);
        const auto keys = achieve.conversationKeys();
        CHECK_TRUE(std::find(keys.begin(), keys.end(), key.toString()) != keys.end());
    }
    // 注册表在：新实例（目录扫描无法反解旧命名）仍能枚举该会话
    {
        Achieve achieve(dir);
        const auto keys = achieve.conversationKeys();
        CHECK_TRUE(std::find(keys.begin(), keys.end(), key.toString()) != keys.end());
    }
    // 注册表丢失：旧命名无法无损反解 → 只留诊断（已文档化的限制）
    std::filesystem::remove(dir / Achieve::registryFileName());
    {
        Achieve achieve(dir);
        CHECK_TRUE(achieve.conversationKeys().empty());
        CHECK_FALSE(achieve.discoveryDiagnostics().empty());
    }
}

// ---------------------------------------------------------------------------
// pendingRange 口径对齐：toMessageId = 范围内最后一条可总结消息
// ---------------------------------------------------------------------------
MIO_TEST(F_pendingRange口径对齐最后一条可总结消息) {
    TempDir tmp("pending_align");
    Achieve achieve(tmp.path / "history");
    const ConversationKey key = privKey("ua");

    CHECK_EQ(achieve.appendChecked(key, mk(Role::User, "u1")).messageId,
             std::int64_t{1});
    CHECK_EQ(achieve.appendChecked(key, mk(Role::Assistant, "a1")).messageId,
             std::int64_t{2});
    Msg tool = mk(Role::Tool, "tool out");
    tool.toolCallId = "c1";
    CHECK_EQ(achieve.appendChecked(key, tool).messageId, std::int64_t{3});
    Msg reasoningOnly = mk(Role::Assistant, "");  // 末尾只有 reasoning：不可总结
    reasoningOnly.reasoningContent = "reasoning";
    CHECK_EQ(achieve.appendChecked(key, reasoningOnly).messageId, std::int64_t{4});

    const Achieve::PendingRange r = achieve.pendingRange(key, 0);
    CHECK_EQ(r.fromMessageId, std::int64_t{1});
    CHECK_EQ(r.toMessageId, std::int64_t{2});  // 不指向工具(3)/纯 reasoning(4)
    CHECK_EQ(r.count, std::int64_t{2});

    // 冻结范围 [from, to] 提交后：之后没有可总结消息 → 空范围（{0,0,0}）
    const Achieve::PendingRange after =
        achieve.pendingRange(key, r.toMessageId);
    CHECK_EQ(after.count, std::int64_t{0});
    CHECK_EQ(after.fromMessageId, std::int64_t{0});
    CHECK_EQ(after.toMessageId, std::int64_t{0});
}

