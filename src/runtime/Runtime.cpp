#include "runtime/Runtime.h"

#include "admin/AdminServer.h"
#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "config/embedding/EmbeddingConfig.h"
#include "config/openai/OpenaiConfig.h"
#include "providers/llm/openai/OpenAiCompat.h"
#include "providers/embedding/openai/OpenAiEmbedding.h"
#include "providers/llm/tool/ToolLoop.h"
#include "providers/mcp/McpManager.h"
#include "log/Log.h"

#include <ctime>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mio {
namespace {

constexpr int kMaxPeopleTokens = 1200;

// 工具（set_public / update_topic / get_current_user_qq 等）需要定位"当前正在处理"的 unit 与其来源；
// 并发 ingest 各自线程互不干扰，故用 thread_local
static thread_local FusionUnit* g_activeUnit = nullptr;
static thread_local ConversationKey g_activeConv;
// 记忆归属上下文：私聊 → 对话者 internalId；群聊 → 空串（按会话共享）。
// 摘要 sink 在 route 内部同步触发，必须先于 route 就位
static thread_local std::string g_activeMemoryPerson;
static thread_local std::string g_activeSenderId;
static thread_local std::string g_activeSenderName;
static thread_local std::string g_activePlatform;
static thread_local std::string g_activeGroupId;
static thread_local bool g_activeKeepSilent = false;

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
                 std::shared_ptr<Llm> llm,
                 std::shared_ptr<ConfigManager> configMgr)
    : persona_{botName, "", "", ""},
      graph_{dataDir / "relationships.json"},
      diary_{dataDir / "diary.json"},
      achieve_{dataDir / "history"},
      eventLog_{dataDir / "events.jsonl"},
      configMgr_(configMgr ? std::move(configMgr) : std::make_shared<ConfigManager>()),
      llm_(llm ? std::move(llm) : std::make_shared<OpenAiCompat>(configMgr_->get()->openai)),
      embedding_(std::make_shared<OpenAiEmbedding>(configMgr_->get()->embedding)),
      summary_(std::make_shared<SummaryManager>(800, llm_)),
      builder_(std::make_shared<ContextBuilder>(configMgr_->get()->contextBuilder, summary_)),
      memoryStore_(dataDir / "memory.db"),
      memory_(configMgr_->get()->memory, embedding_, memoryStore_),
      router_{graph_, achieve_, summary_, configMgr_->get()->fusion, embedding_},
      inputBuffers_{configMgr_->get()->inputBuffer} {
    log::initFromEnvironment();  // MIO_LOG_LEVEL，重复调用安全

    if (configMgr_ && configMgr_->get()) {
        auto cfg = configMgr_->get();
        if (!cfg->botName.empty() && botName == "Mio") persona_.botName = cfg->botName;
        if (!cfg->character.empty()) persona_.character = cfg->character;
        if (!cfg->systemPromptPrefix.empty()) persona_.systemPromptPrefix = cfg->systemPromptPrefix;
        if (!cfg->systemPromptNotice.empty()) persona_.systemPromptNotice = cfg->systemPromptNotice;
    }

    registerBuiltinTools();

    // MCP 动态扩展工具接入
    mcpManager_ = std::make_unique<mcp::McpManager>();
    if (configMgr_ && configMgr_->get()) {
        mcpManager_->initialize(configMgr_->get()->mcp);
        mcpManager_->syncTools(registry_);
    }

    // MIO 也是 person（统一图谱结构；隐私差异化靠装配/检索时隐藏数据）
    graph_.ensureMio(persona_.botName);

    // 写入流程接线：三条摘要路径（冷读取/融合重定向/重新冷启动）共用
    // SummaryManager 这一个咽喉点，产出即写入长期记忆
    summary_->setOnSummary(
        [this](const SummaryOutcome& outcome) { onSummaryProduced(outcome); });

    systemPrompt_ = rebuildSystemPrompt();
    state_.botName = persona_.botName;
    state_.messageCount = 0;

    int adminPort = (configMgr_ && configMgr_->get()) ? configMgr_->get()->adminPort : 6188;
    if (adminPort > 0) {
        adminServer_ = std::make_unique<AdminServer>(*this, adminPort);
        adminServer_->start();
    }
}

Runtime::Runtime(std::string botName, std::filesystem::path dataDir,
                 std::unique_ptr<Llm> llm)
    : Runtime(std::move(botName), std::move(dataDir),
              std::shared_ptr<Llm>(std::move(llm))) {}

Runtime::~Runtime() {
    if (mcpManager_) {
        mcpManager_->stop();
        mcpManager_.reset();
    }
    if (adminServer_) {
        adminServer_->stop();
        adminServer_.reset();
    }
}

bool Runtime::reloadConfig(const std::filesystem::path& configPath) {
    if (!configMgr_) {
        log::warn("Runtime", "ConfigManager 未初始化");
        return false;
    }

    if (!configMgr_->reloadFromFile(configPath)) {
        log::warn("Runtime", "配置重载失败，保持当前配置运行: " + configPath.string());
        return false;
    }

    auto newConfig = configMgr_->get();
    try {
        auto newLlm = std::make_shared<OpenAiCompat>(newConfig->openai);
        auto newEmbedding = std::make_shared<OpenAiEmbedding>(newConfig->embedding);
        auto newSummary = std::make_shared<SummaryManager>(800, newLlm);
        newSummary->setOnSummary([this](const SummaryOutcome& o) { onSummaryProduced(o); });
        auto newBuilder = std::make_shared<ContextBuilder>(newConfig->contextBuilder, newSummary);

        // 重载时清理动态工具并重新热加载 MCP 服务
        registry_.clearDynamicTools();
        if (mcpManager_) {
            mcpManager_->reload(newConfig->mcp, registry_);
        }

        // 原子切换服务指针与更新提示词
        {
            std::unique_lock<std::shared_mutex> lock(servicesMtx_);
            llm_ = newLlm;
            embedding_ = newEmbedding;
            summary_ = newSummary;
            builder_ = newBuilder;

            if (!newConfig->botName.empty()) persona_.botName = newConfig->botName;
            if (!newConfig->character.empty()) persona_.character = newConfig->character;
            if (!newConfig->systemPromptPrefix.empty())
                persona_.systemPromptPrefix = newConfig->systemPromptPrefix;
            if (!newConfig->systemPromptNotice.empty())
                persona_.systemPromptNotice = newConfig->systemPromptNotice;
            systemPrompt_ = rebuildSystemPrompt();
        }

        // 刷新长驻组件配置与依赖
        router_.update(newConfig->fusion, newSummary, newEmbedding);
        memory_.update(newConfig->memory, newEmbedding);
        inputBuffers_.updateConfig(newConfig->inputBuffer);

        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            state_.botName = persona_.botName;
        }

        const std::int64_t now = std::time(nullptr);
        eventLog_.append({0, EventKind::ConfigReloaded, now, "配置热重载: " + configPath.string()});
        log::info("Runtime", "配置热重载成功: " + configPath.string());
        return true;
    } catch (const std::exception& e) {
        log::warn("Runtime", std::string("组件重建异常，保持原配置: ") + e.what());
        return false;
    }
}

std::shared_ptr<const AppConfig> Runtime::config() const {
    return configMgr_ ? configMgr_->get() : nullptr;
}

std::shared_ptr<ConfigManager> Runtime::configManager() const {
    return configMgr_;
}

std::string Runtime::systemPrompt() const {
    std::shared_lock<std::shared_mutex> lock(servicesMtx_);
    return systemPrompt_;
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
                  }, ToolLayer::Builtin);

    // remember —— 存储具有长期价值的事实或背景到长期记忆库（供后续 recall_memory 检索回忆）
    ToolDef rememberDef;
    rememberDef.name = "remember";
    rememberDef.description =
        "将具有长期价值的对话事实、用户喜好、重要事件或约定存储到长期记忆库中。后续可通过 recall_memory 检索回忆。";
    rememberDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"text", {{"type", "string"}, {"description", "要存储的事实或记忆内容"}}},
          {"is_public",
           {{"type", "boolean"},
            {"description",
             "是否对其他会话公开（可选，默认随当前会话私密性）"}}}}},
        {"required", nlohmann::json::array({"text"})}};
    registry_.add(
        std::move(rememberDef),
        [this](const nlohmann::json& args) -> std::string {
            const std::string text = args.value("text", "");
            if (text.empty()) return "error: 记忆内容不能为空";
            const std::int64_t now = std::time(nullptr);
            bool isPublic = g_activeUnit ? g_activeUnit->isPublic() : false;
            if (args.contains("is_public") && args["is_public"].is_boolean()) {
                isPublic = args["is_public"].get<bool>();
            }
            memory_.remember(g_activeConv, g_activeMemoryPerson, text, isPublic, now);
            eventLog_.append({0, EventKind::MemoryRemembered, now, text});
            return "已存入长期记忆库。";
        },
        ToolLayer::Builtin);

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
                          return "已更新昵称。";
                      }
                      return "error: 找不到该昵称对应的人。";
                  }, ToolLayer::Builtin);

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
                          return "已更新印象。";
                      }
                      return "error: 找不到该昵称对应的人。";
                  }, ToolLayer::Builtin);

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
                  }, ToolLayer::Builtin);

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
                  }, ToolLayer::Builtin);

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
                  }, ToolLayer::Builtin);

    // get_current_user_qq —— 获取当前对话/发言人的QQ号及会话信息
    ToolDef curUserDef;
    curUserDef.name = "get_current_user_qq";
    curUserDef.description =
        "获取当前正在对话/发言用户的QQ号、昵称及会话信息（如私聊或群号）。";
    curUserDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}};
    registry_.add(std::move(curUserDef),
                  [this](const nlohmann::json& /*args*/) -> std::string {
                      if (g_activeSenderId.empty()) {
                          return "error: 当前无可用的对话上下文";
                      }
                      nlohmann::json info;
                      info["platform"] = g_activePlatform;
                      info["user_id"] = g_activeSenderId;
                      info["nickname"] = g_activeSenderName;
                      std::string qq = (g_activePlatform == "qq" ||
                                        g_activePlatform == "napcat" ||
                                        g_activePlatform == "onebot")
                                           ? g_activeSenderId
                                           : "";
                      if (const PersonNode* p = graph_.findByPlatform(
                              g_activePlatform, g_activeSenderId);
                          p != nullptr) {
                          info["graph_name"] = p->name;
                          if (qq.empty()) {
                              qq = RelationshipGraph::extractQq(*p);
                          }
                      }
                      info["qq"] = qq.empty() ? "未知" : qq;
                      info["scope"] = (g_activeConv.scope == ConversationScope::Group)
                                          ? "group"
                                          : "private";
                      if (g_activeConv.scope == ConversationScope::Group) {
                          info["group_id"] = !g_activeGroupId.empty()
                                                 ? g_activeGroupId
                                                 : g_activeConv.id;
                      }
                      return info.dump();
                  },
                  ToolLayer::Builtin);

    // get_known_person_qq —— 获取认识的人的QQ号
    ToolDef knownPersonDef;
    knownPersonDef.name = "get_known_person_qq";
    knownPersonDef.description =
        "获取认识的人的QQ号。可输入姓名/昵称查询特定人物；若不提供参数则列出所有已知人物及其QQ号。";
    knownPersonDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"description",
             "要查询的人物姓名或称呼（可选，若为空则列出所有认识的人）"}}}}}};
    registry_.add(std::move(knownPersonDef),
                  [this](const nlohmann::json& args) -> std::string {
                      std::string queryName = args.value("name", "");
                      if (!queryName.empty()) {
                          const PersonNode* p = graph_.findByName(queryName);
                          if (!p) {
                              auto all = graph_.allPersons();
                              for (const auto* node : all) {
                                  if (node && (node->name.find(queryName) != std::string::npos ||
                                               node->personal.find(queryName) != std::string::npos)) {
                                      p = node;
                                      break;
                                  }
                              }
                          }
                          if (!p) {
                              return "未找到名为 \"" + queryName + "\" 的人。";
                          }
                          std::string qq = RelationshipGraph::extractQq(*p);
                          nlohmann::json result;
                          result["name"] = p->name;
                          result["qq"] = qq.empty() ? "未知" : qq;
                          result["platform_ids"] = p->platformIds;
                          if (!p->personal.empty()) result["personal"] = p->personal;
                          return result.dump();
                      }

                      auto persons = graph_.allPersons();
                      nlohmann::json list = nlohmann::json::array();
                      for (const auto* p : persons) {
                          if (!p) continue;
                          std::string qq = RelationshipGraph::extractQq(*p);
                          nlohmann::json item;
                          item["name"] = p->name;
                          item["qq"] = qq.empty() ? "未知" : qq;
                          item["platform_ids"] = p->platformIds;
                          if (!p->personal.empty()) item["personal"] = p->personal;
                          list.push_back(std::move(item));
                      }
                      return list.dump();
                  },
                  ToolLayer::Builtin);

    // keepsilent —— 保持沉默（不想回答 / 已读不回 / 拒绝回复）
    ToolDef keepSilentDef;
    keepSilentDef.name = "keepsilent";
    keepSilentDef.description =
        "当你阅读了用户的话后，内心感到被冒犯不想回复、想要已读不回、或者认为此时保持沉默更符合你的心情时，调用此工具保持沉默。调用后你不会向用户发送任何消息。";
    keepSilentDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"reason",
           {{"type", "string"},
            {"description", "决定保持沉默的内心理由（如：被惹毛了不想说话/觉得对方很无聊）"}}}}},
        {"required", nlohmann::json::array({"reason"})}};
    keepSilentDef.isTerminal = true;
    registry_.add(
        keepSilentDef,
        [](const nlohmann::json& args) -> std::string {
            g_activeKeepSilent = true;
            std::string reason = args.value("reason", "");
            log::info("Runtime", "模型调用 keepsilent 保持沉默: " + reason);
            return "已选择保持沉默。本轮不会向用户发送任何回复。";
        },
        ToolLayer::Builtin);

    // keep_silent 别名
    ToolDef keepSilentAliasDef = keepSilentDef;
    keepSilentAliasDef.name = "keep_silent";
    keepSilentAliasDef.description = "同 keepsilent，在不想回答或决定已读不回时调用。";
    registry_.add(
        std::move(keepSilentAliasDef),
        [](const nlohmann::json& args) -> std::string {
            g_activeKeepSilent = true;
            std::string reason = args.value("reason", "");
            log::info("Runtime", "模型调用 keep_silent 保持沉默: " + reason);
            return "已选择保持沉默。本轮不会向用户发送任何回复。";
        },
        ToolLayer::Builtin);

    // stay_silent 别名
    ToolDef staySilentDef = keepSilentDef;
    staySilentDef.name = "stay_silent";
    staySilentDef.description = "同 keepsilent，在不想回答或决定已读不回时调用。";
    registry_.add(
        std::move(staySilentDef),
        [](const nlohmann::json& args) -> std::string {
            g_activeKeepSilent = true;
            std::string reason = args.value("reason", "");
            log::info("Runtime", "模型调用 stay_silent 保持沉默: " + reason);
            return "已选择保持沉默。本轮不会向用户发送任何回复。";
        },
        ToolLayer::Builtin);
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

    // 捕获当前代际的服务快照（RCU 模式），确保单次调用内部的组件生命周期一致
    std::shared_ptr<Llm> currentLlm;
    std::shared_ptr<ContextBuilder> currentBuilder;
    std::shared_ptr<SummaryManager> currentSummary;
    std::string currentSystemPrompt;
    {
        std::shared_lock<std::shared_mutex> lock(servicesMtx_);
        currentLlm = llm_;
        currentBuilder = builder_;
        currentSummary = summary_;
        currentSystemPrompt = systemPrompt_;
    }
    if (!currentLlm || !currentBuilder) {
        throw std::runtime_error("Runtime 服务未就绪");
    }

    // 1) 物理通道层防抖：获取当前物理会话专属的 InputBuffer
    auto buffer = inputBuffers_.getOrCreate(message.conversation);
    const std::string displayName = router_.displayName(message);
    const PushResult pushRes = buffer->push(message, displayName, now);
    if (pushRes == PushResult::Rejected) {
        return BotReply{message.conversation, ""};
    }
    if (pushRes == PushResult::AcceptedFollower) {
        // 作为跟随消息已合流进当前物理会话等待批次，静默返回，由 Leader 统一处理
        return BotReply{message.conversation, ""};
    }

    // 当前线程为该物理会话的 Leader，等待 0.5~1.0s 防抖窗口结束，获取当前批次所有消息
    BufferedBatch batch = buffer->waitForBatch();
    if (batch.empty()) {
        return BotReply{message.conversation, ""};
    }

    // 2) 设置当前线程的记忆归属与对话上下文（冷读取摘要 sink 在 route 内部同步触发）
    const auto& targetMsg = batch.rawMessages.back();
    g_activeMemoryPerson = resolveMemoryPerson(targetMsg);
    g_activeConv = targetMsg.conversation;
    g_activeSenderId = targetMsg.senderId;
    g_activeSenderName = targetMsg.senderName;
    g_activePlatform = targetMsg.platform;
    g_activeGroupId = targetMsg.groupId;
    g_activeKeepSilent = false;

    struct ContextGuard {
        ~ContextGuard() {
            g_activeUnit = nullptr;
            g_activeConv = {};
            g_activeMemoryPerson.clear();
            g_activeSenderId.clear();
            g_activeSenderName.clear();
            g_activePlatform.clear();
            g_activeGroupId.clear();
            g_activeKeepSilent = false;
        }
    } contextGuard;

    // 3) 路由与关系图谱登记：批次内每条原始消息进行 route 处理（登记 @ 关系、亲密度等）
    FusionUnit* unit = nullptr;
    for (const auto& raw : batch.rawMessages) {
        unit = router_.route(raw, now);
    }
    if (!unit) {
        return BotReply{batch.conversation, ""};
    }
    g_activeUnit = unit;

    ChatResponse resp;
    bool reColdStarted = false;
    {
        // 上下文级锁：同 unit 串行（含 LLM 往返），跨 unit 并行
        std::lock_guard<std::mutex> lock(unit->mtx);

        // 4) 批次内所有原始消息分别落入物理会话的真实档案
        for (const auto& raw : batch.rawMessages) {
            Msg rawTurn{Role::User};
            rawTurn.text = raw.text;
            rawTurn.parts = raw.parts;  // 图片等多模态片段随原始消息落档案
            rawTurn.createdAt = now;
            rawTurn.senderId = raw.senderId;
            rawTurn.senderName = router_.displayName(raw);
            rawTurn.platform = raw.platform;
            rawTurn.groupId = raw.groupId;
            achieve_.append(batch.conversation, rawTurn);
        }

        // 5) 批次内重组好的独立上下文 turns 追加进 unit 上下文
        for (auto& turn : batch.turns) {
            unit->append(std::move(turn));
        }

        // 6) 构建请求（当前回合已 append 进 unit；上下文不够 → 重新冷启动压缩）
        BuildInput in;
        in.systemPrompt = currentSystemPrompt;
        in.unit = unit;
        in.now = now;
        auto result = currentBuilder->build(in);
        reColdStarted = result.reColdStarted;

        ChatRequest req = result.request;
        req.tools = registry_.defs();

        // 7) 工具循环；sink：逐条落档案 + 入 unit 上下文
        resp = runToolLoop(*currentLlm, registry_, req, ToolLoopOptions{},
                           [this, unit, conv = batch.conversation](
                                const Msg& m) {
                                achieve_.append(conv, m);
                                unit->append(m);
                            });
    }
    // 8) 锁外：状态快照 + 审计 + Facts 重建
    for (const auto& raw : batch.rawMessages) {
        noteMessage(raw.conversation, raw.senderId);
    }
    if (reColdStarted) {
        eventLog_.append({0, EventKind::SummaryApplied, now, "上下文重新冷启动"});
        std::unique_lock<std::shared_mutex> lock(servicesMtx_);
        systemPrompt_ = rebuildSystemPrompt(diary_.consume());
    }

    if (g_activeKeepSilent) {
        log::info("Runtime", "模型自主调用 keepsilent 保持沉默，本轮不发送回复消息");
        resp.text = "";
    }

    return BotReply{batch.conversation, resp.text};
}

RuntimeState Runtime::state() const {
    std::lock_guard<std::mutex> lock(stateMtx_);
    return state_;
}

} // namespace mio
