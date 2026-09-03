#include "runtime/Runtime.h"

#include "config/embedding/EmbeddingConfig.h"
#include "config/openai/OpenaiConfig.h"
#include "llm/openai/OpenAiCompat.h"
#include "llm/openai/OpenAiEmbedding.h"
#include "llm/tool/ToolLoop.h"
#include "log/Log.h"

#include <ctime>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mio {
namespace {

constexpr int kMaxPeopleTokens = 1200;

// 工具（set_public / update_topic）需要定位"当前正在处理"的 unit 与其来源；
// 并发 ingest 各自线程互不干扰，故用 thread_local
static thread_local FusionUnit* g_activeUnit = nullptr;
static thread_local ConversationKey g_activeConv;
// 记忆归属上下文：私聊 → 对话者 internalId；群聊 → 空串（按会话共享）。
// 摘要 sink 在 route 内部同步触发，必须先于 route 就位
static thread_local std::string g_activeMemoryPerson;

std::unique_ptr<Llm> defaultLlm() {
    return std::make_unique<OpenAiCompat>(OpenAiConfig::fromEnvironment());
}

std::string hexEncode(const std::string& s) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 0x0F];
    }
    return out;
}

} // namespace

Runtime::Runtime(std::string botName, std::filesystem::path dataDir,
                 std::unique_ptr<Llm> llm)
    // 人设与基础事实；character 将来从配置文件来
    : persona_{botName,
               "你说话简短、有点毒舌但其实很关心对方，喜欢在句尾加 \"喵~\"。"},
      graph_{dataDir / "relationships.json"},
      diary_{dataDir / "diary.json"},
      achieve_{dataDir / "history"},
      eventLog_{dataDir / "events.jsonl"},
      llm_(llm ? std::move(llm) : defaultLlm()),
      embedding_(std::make_unique<OpenAiEmbedding>(
          EmbeddingConfig::fromEnvironment())),
      memoryStore_(dataDir / "memory.db"),
      memory_(MemoryConfig{}, *embedding_, memoryStore_),
      summary_{800, *llm_},
      router_{graph_, achieve_, summary_, {}, embedding_.get()},
      builder_{ContextBuilderConfig{}, summary_} {
    log::initFromEnvironment();  // MIO_LOG_LEVEL，重复调用安全
    registerBuiltinTools();

    // MIO 也是 person（统一图谱结构；隐私差异化靠装配/检索时隐藏数据）
    graph_.ensureMio(persona_.botName);

    // 写入流程接线：三条摘要路径（冷读取/融合重定向/重新冷启动）共用
    // SummaryManager 这一个咽喉点，产出即写入长期记忆
    summary_.setOnSummary(
        [this](const SummaryOutcome& outcome) { onSummaryProduced(outcome); });

    systemPrompt_ = rebuildSystemPrompt();
    state_.botName = std::move(botName);
    state_.messageCount = 0;
}

void Runtime::registerBuiltinTools() {
    // get_current_time —— 故意选一个"模型绝对不可能自己知道答案"的工具，
    // 用于验证工具链路是否打通。
    ToolDef timeDef;
    timeDef.name = "get_current_time";
    timeDef.description = "获取当前的本地日期与时间（精确到分钟）。";
    timeDef.parametersJsonSchema = {{"type", "object"},
                                    {"properties", nlohmann::json::object()}};
    registry_.add(std::move(timeDef),
                  [](const nlohmann::json&) -> std::string {
                      return "当前时间: " + nowTimeString();
                  });

    // write_diary —— 模型基础认知的进步记录（认识新人/事实刷新时写）
    ToolDef diaryDef;
    diaryDef.name = "write_diary";
    diaryDef.description =
        "把有长期价值的认知写入日记：认识了新的人、人物关系变化、"
        "用户明确陈述的重要事实。只在值得长期记住时调用。";
    diaryDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"text", {{"type", "string"}, {"description", "要记录的认知内容"}}}}},
        {"required", nlohmann::json::array({"text"})}};
    registry_.add(std::move(diaryDef),
                  [this](const nlohmann::json& args) -> std::string {
                      const std::string text = args.value("text", "");
                      const std::int64_t now = std::time(nullptr);
                      diary_.add(text, now);
                      eventLog_.append({0, EventKind::DiaryWritten, now, text});
                      return "已写入日记。";
                  });

    // set_nickname —— 模型纠正对人的称呼（只改称呼，不改身份映射）
    ToolDef nickDef;
    nickDef.name = "set_nickname";
    nickDef.description = "修改你对某个人的称呼昵称。";
    nickDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"current", {{"type", "string"}, {"description", "当前昵称"}}},
          {"nickname", {{"type", "string"}, {"description", "新昵称"}}}}},
        {"required", nlohmann::json::array({"current", "nickname"})}};
    registry_.add(std::move(nickDef),
                  [this](const nlohmann::json& args) -> std::string {
                      if (graph_.setNickname(args.value("current", ""),
                                             args.value("nickname", ""))) {
                          const std::int64_t now = std::time(nullptr);
                          eventLog_.append({0, EventKind::NicknameChanged, now,
                                            args.value("current", "") + " → " +
                                                args.value("nickname", "")});
                          //systemPrompt_ = rebuildSystemPrompt();
                          return "已更新昵称。";
                      }
                      return "error: 找不到该昵称对应的人。";
                  });

    // set_notes —— 模型补充对某个人的印象（personal）
    ToolDef notesDef;
    notesDef.name = "set_notes";
    notesDef.description = "补充/修改你对某个人的印象描述（如喜好、特点）。";
    notesDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"person", {{"type", "string"}, {"description", "昵称"}}},
          {"notes", {{"type", "string"}, {"description", "印象描述"}}}}},
        {"required", nlohmann::json::array({"person", "notes"})}};
    registry_.add(std::move(notesDef),
                  [this](const nlohmann::json& args) -> std::string {
                      if (graph_.setNotes(args.value("person", ""),
                                          args.value("notes", ""))) {
                          const std::int64_t now = std::time(nullptr);
                          eventLog_.append({0, EventKind::NotesChanged, now,
                                            args.value("person", "") + ": " +
                                                args.value("notes", "")});
                          //systemPrompt_ = rebuildSystemPrompt();
                          return "已更新印象。";
                      }
                      return "error: 找不到该昵称对应的人。";
                  });

    // set_public —— 模型控制当前上下文是否公开。
    // 直接切换当前 unit 的 isPublic，方便模型表达“这段对话是否适合融合”。
    ToolDef publicDef;
    publicDef.name = "set_public";
    publicDef.description =
        "在会话内容转为私密时调用，只有在developer命令时可设置为公开";
    publicDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"is_public", {{"type", "boolean"},
                         {"description", "true=公开，false=私密"}}},
          {"reason", {{"type", "string"},
                      {"description", "设置公开/私密的原因（调试用）"}}}}},
        {"required", nlohmann::json::array({"is_public"})}};
    registry_.add(std::move(publicDef),
                  [this](const nlohmann::json& args) -> std::string {
                      if (!g_activeUnit) return "error: 无当前上下文";
                      bool isPublic = false;
                      if (args.contains("is_public")) {
                          const auto& v = args["is_public"];
                          if (v.is_boolean()) {
                              isPublic = v.get<bool>();
                          } else if (v.is_string()) {
                              const std::string s = v.get<std::string>();
                              isPublic = (s == "true" || s == "1" || s == "公开");
                          }
                      }
                      const std::string reason = args.value("reason", "");
                      log::info("Runtime",
                                "set_public 调用: conv=" + g_activeConv.toString() +
                                    " is_public=" + (isPublic ? "true" : "false") +
                                    " reason=" + reason +
                                    " topic=" + g_activeUnit->topic() +
                                    " isPublic=" +
                                    (g_activeUnit->isPublic() ? "true" : "false"));
                      FusionUnit* target = router_.setPublic(g_activeConv, isPublic);
                      if (!target) return "error: 无法定位当前上下文";
                      g_activeUnit = target;
                      return isPublic ? "已设为公开。" : "已转为私密。";
                  });

    // update_topic —— 话题变化上报，供 Router 判断融合
    ToolDef topicDef;
    topicDef.name = "update_topic";
    topicDef.description =
        "报告当前对话的话题（话题发生明显转变时调用），话题稳定后调用，不要频繁调用";
    topicDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"topic", {{"type", "string"}, {"description", "当前话题"}}}}},
        {"required", nlohmann::json::array({"topic"})}};
    registry_.add(std::move(topicDef),
                  [this](const nlohmann::json& args) -> std::string {
                      if (!g_activeUnit) return "error: 无当前上下文";
                      const std::string oldTopic = g_activeUnit->topic();
                      const std::string newTopic = args.value("topic", "");
                      g_activeUnit->setTopic(newTopic);
                      
                      log::info("Runtime",
                                "update_topic: conv=" + g_activeConv.toString() +
                                    " old=" + oldTopic + " new=" + newTopic +
                                    " hex=" + hexEncode(newTopic) +
                                    " isPublic=" +
                                    (g_activeUnit->isPublic() ? "true" : "false"));
                      return "已更新话题:" + newTopic + "\n";
                  });

    // recall_memory —— 长期记忆召回由模型自行决定何时调用。
    // 之前是每轮自动把 Top-K 塞进 user 消息，现在改为工具化，
    // 模型只在需要回忆旧事、或判断当前话题与历史相关时主动检索。
    ToolDef recallDef;
    recallDef.name = "recall_memory";
    recallDef.description =
        "检索长期记忆中的历史经历摘要。当用户提到过去的事、你记不清旧对话、"
        "或需要结合历史背景回答时调用。";
    recallDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"query", {{"type", "string"},
                     {"description", "检索关键词或自然语言问题，例如：用户喜欢什么"}}},
          {"top_k", {{"type", "integer"},
                     {"description", "返回条数，默认 3，最多 10"}}}}},
        {"required", nlohmann::json::array({"query"})}};
    registry_.add(std::move(recallDef),
                  [this](const nlohmann::json& args) -> std::string {
                      const std::string query = args.value("query", "");
                      if (query.empty()) return "error: query 不能为空";
                      std::size_t topK = 0;
                      if (args.contains("top_k") &&
                          args["top_k"].is_number_integer()) {
                          const int raw = args["top_k"].get<int>();
                          topK = raw > 0 ? static_cast<std::size_t>(raw) : 0;
                          if (topK > 10) topK = 10;
                      }
                      auto recalled = memory_.recall(
                          query, g_activeMemoryPerson, g_activeConv.toString(),
                          std::time(nullptr), topK);
                      if (recalled.empty()) return "没有找到相关记忆。";
                      return MemoryManager::renderForPrompt(recalled);
                  });
}

std::string Runtime::rebuildSystemPrompt(
    const std::vector<DiaryEntry>& cognition) {
    // 只读已封存认知（由摘要路径提供），保护 prompt cache 前缀
    return buildSystemPrompt(persona_, graph_, cognition, kMaxPeopleTokens);
}

void Runtime::updateState(const Event& ev) {
    std::lock_guard<std::mutex> lock(stateMtx_);
    state_.lastEventSeq = ev.seq;
}

void Runtime::noteMessage(const ConversationKey& conversation,
                          const std::string& senderId) {
    std::lock_guard<std::mutex> lock(stateMtx_);
    state_.messageCount++;
    if (!senderId.empty()) state_.activeUsers.insert(senderId);
    state_.activeConversation = conversation;
}

std::string Runtime::resolveMemoryPerson(const IncomingMessage& msg) const {
    // 私聊 → 对话者 internalId：私密记忆跨会话跟随本人（同人跨平台可召回）；
    // 群聊 → 空：没有单一归属人，群记忆按 conv_key 在会话内共享
    if (msg.conversation.scope != ConversationScope::Private) return "";
    if (const PersonNode* p = graph_.findByPlatform(msg.platform, msg.senderId);
        p != nullptr)
        return p->internalId;
    return "";
}

void Runtime::onSummaryProduced(const SummaryOutcome& outcome) {
    if (outcome.text.empty()) return;
    // privateVerdict 语义：true = 私密 → 记忆 is_public 取反。
    // 会话/归属读 thread_local（sink 由 summarizeWithVerdict 在 ingest
    // 线程内同步触发，上下文必然就位）
    memory_.remember(g_activeConv, g_activeMemoryPerson, outcome.text,
                     !outcome.privateVerdict, std::time(nullptr));
}

BotReply Runtime::ingest(IncomingMessage message) {
    const std::int64_t now = std::time(nullptr);

    // 0) 记忆归属上下文先于 route 就位（冷读取摘要的 sink 在 route 内触发）
    g_activeMemoryPerson = resolveMemoryPerson(message);
    g_activeConv = message.conversation;

    // 1) 路由：身份映射 → 首次遇见 Achieve 冷读取建 unit → 融合判断（redirect）→ 目标 unit
    FusionUnit* unit = router_.route(message, now);
    g_activeUnit = unit;

    Msg turn{Role::User};
    turn.text = message.text;
    turn.createdAt = now;
    turn.senderId = message.senderId;   // 原始平台 id：档案/身份归一用
    turn.senderName = router_.displayName(message);  // Router 身份注入：图谱昵称优先
    turn.platform = message.platform;
    turn.groupId = message.groupId;

    // 2) 记忆召回改为工具化：模型需要历史背景时自行调用 recall_memory，
    //    不再每轮自动把 Top-K 注入 user 消息。
    //    （召回仍使用 thread_local 的 g_activeMemoryPerson / g_activeConv 做权限过滤）

    ChatResponse resp;
    bool reColdStarted = false;
    {
        // 上下文级锁：同 unit 串行（含 LLM 往返），跨 unit 并行
        std::lock_guard<std::mutex> lock(unit->mtx);

        // 3) 落档案（事实源）+ 入 unit 上下文
        achieve_.append(message.conversation, turn);
        unit->append(turn);

        // 4) 构建请求（当前回合已 append 进 unit；上下文不够 → 重新冷启动压缩）
        BuildInput in;
        in.systemPrompt = systemPrompt_;
        in.unit = unit;
        in.now = now;
        auto result = builder_.build(in);
        reColdStarted = result.reColdStarted;

        ChatRequest req = result.request;
        req.tools = registry_.defs();

        // 5) 工具循环；sink：逐条落档案 + 入 unit 上下文
        resp = runToolLoop(*llm_, registry_, req, ToolLoopOptions{},
                           [this, unit, conv = message.conversation](
                               const Msg& m) {
                               achieve_.append(conv, m);
                               unit->append(m);
                           });
    }
    // 6) 锁外：状态快照 + 审计 + Facts 重建
    noteMessage(message.conversation, message.senderId);
    if (reColdStarted) {
        eventLog_.append({0, EventKind::SummaryApplied, now, "上下文重新冷启动"});
        systemPrompt_ = rebuildSystemPrompt(diary_.consume());
    }
    g_activeUnit = nullptr;
    g_activeConv = {};
    g_activeMemoryPerson.clear();

    return BotReply{message.conversation, resp.text};
}

RuntimeState Runtime::state() const {
    std::lock_guard<std::mutex> lock(stateMtx_);
    return state_;
}

} // namespace mio
