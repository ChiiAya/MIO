#include "runtime/Runtime.h"

#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "config/embedding/EmbeddingConfig.h"
#include "config/openai/OpenaiConfig.h"
#include "providers/llm/openai/OpenAiCompat.h"
#include "providers/embedding/openai/OpenAiEmbedding.h"
#include "providers/llm/tool/ToolLoop.h"
#include "providers/llm/tool/handlers/MemoryToolHandlers.h"
#include "providers/memory/MemoryProviderFactory.h"
#include "admin/AdminServer.h"
#include "log/Log.h"

#include <algorithm>
#include <chrono>
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
static thread_local std::string g_activeMemoryPerson;
static thread_local std::string g_activeSenderId;
static thread_local std::string g_activeSenderName;
static thread_local std::string g_activePlatform;
static thread_local std::string g_activeGroupId;
static thread_local bool g_activeKeepSilent = false;
// 服务端权限上下文：只服务于【同步】工具调用路径（ingest 线程内 ToolLoop → handler）。
// 后台工作（摘要/向量化/插件）MUST NOT 读取它 —— 那些路径显式携带
// SummaryJob / AccessContext，归属不从 thread_local 推断。
static thread_local AccessContext g_accessCtx;

constexpr std::size_t kMaxParticipantsInJob = 64;

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
    // 人设与基础事实；character 将来从配置文件来
    : persona_{botName,
               "你说话简短、有点毒舌但其实很关心对方，喜欢在句尾加 \"喵~\"。"},
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
      router_{graph_, achieve_, summary_, configMgr_->get()->fusion, embedding_} {
    log::initFromEnvironment();  // MIO_LOG_LEVEL，重复调用安全
    registerBuiltinTools();

    // MIO 也是 person（统一图谱结构；隐私差异化靠装配/检索时隐藏数据）
    graph_.ensureMio(persona_.botName);

    // ---- 记忆系统改造：事实/认知/提案存储（C）+ 会话生命周期（A）----------
    proposalStore_ = std::make_unique<ProposalStore>(dataDir / "proposals.json");
    factStore_ = std::make_unique<FactStore>(dataDir / "facts.json");
    cognitionStore_ = std::make_unique<CognitionStore>(dataDir / "cognitions.json");

    // 经历记忆后端（E）：工厂永不返回 nullptr、永不抛；不可用时降级为无长期召回
    memoryProvider_ = makeMemoryProvider(configMgr_->get()->memory.backend, memory_,
                                         configMgr_->get()->memory);
    if (memoryProvider_ && !memoryProvider_->available()) {
        log::warn("Runtime", "经历记忆后端不可用，降级为无长期召回运行: " +
                                 memoryProvider_->name());
    }

    // 静默调度器：pull 模型，由维护线程周期驱动；状态落盘保证重启不刷新重试额度
    lifecycle_ = std::make_shared<ConversationLifecycle>(
        configMgr_->get()->conversation, std::make_shared<SystemClock>(),
        dataDir / "conversation_lifecycle.json");
    lifecycle_->setPendingRangeProvider(
        [this](const std::string& conversationKey, std::int64_t afterMessageId) {
            PendingRange range;  // 与 Achieve::PendingRange 同构（字段直接拷贝）
            const auto conv = ConversationKey::fromString(conversationKey);
            if (!conv.has_value()) return range;
            const Achieve::PendingRange r =
                achieve_.pendingRange(*conv, afterMessageId);
            range.fromMessageId = r.fromMessageId;
            range.toMessageId = r.toMessageId;
            range.count = r.count;
            if (range.count > 0) {
                range.participants = collectParticipants(conversationKey,
                                                         range.fromMessageId,
                                                         range.toMessageId);
            }
            return range;
        });
    lifecycle_->load();

    if (configMgr_ && configMgr_->get()) {
        if (!configMgr_->get()->character.empty()) {
            persona_.character = configMgr_->get()->character;
        }
        if (!configMgr_->get()->botName.empty()) {
            persona_.botName = configMgr_->get()->botName;
        }
        if (!configMgr_->get()->systemPromptPrefix.empty()) {
            persona_.systemPromptPrefix = configMgr_->get()->systemPromptPrefix;
        }
        if (!configMgr_->get()->systemPromptNotice.empty()) {
            persona_.systemPromptNotice = configMgr_->get()->systemPromptNotice;
        }
    }

    // 冷启动数据源：ContextBuilder 的兜底冷启动（不调用摘要模型、不写长期记忆）
    builder_->setColdStartSource(&achieve_);

    // 记忆/会话工具（D）：所有者、当前会话、审核状态一律取自服务端上下文
    registerMemoryTools();

    systemPrompt_ = rebuildSystemPrompt();
    state_.botName = persona_.botName;
    state_.messageCount = 0;

    startMaintenance();

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
    // 关闭顺序（文档要求）：先停调度并回收在飞任务，再释放 store/LLM/服务。
    // stopMaintenance 会 join 维护线程，因此在飞摘要不会在成员析构后继续访问。
    stopMaintenance();
    if (adminServer_) {
        adminServer_->stop();
        adminServer_.reset();
    }
    if (lifecycle_) lifecycle_->persist();  // 重试额度/游标落盘，重启不刷新
}

bool Runtime::reloadConfig(const std::filesystem::path& configPath) {
    if (!configMgr_) {
        log::warn("Runtime", "ConfigManager 未初始化");
        return false;
    }

    // 1) 解析 + 校验候选配置（不发布）：类型/范围错误时整次重载失败
    AppConfig candidate;
    std::string error;
    if (!configMgr_->loadCandidateFromFile(configPath, candidate, &error)) {
        log::warn("Runtime", "配置重载失败，保持当前配置运行: " + error);
        return false;
    }
    if (const auto v = validateConversationLifecycle(candidate.conversation); !v.ok) {
        log::warn("Runtime", "会话生命周期配置非法，保持当前配置: " + v.error);
        return false;
    }

    // 2) 先构造全部依赖；任何异常都不得发布新配置
    std::shared_ptr<Llm> newLlm;
    std::shared_ptr<Embedding> newEmbedding;
    std::shared_ptr<SummaryManager> newSummary;
    std::shared_ptr<ContextBuilder> newBuilder;
    try {
        newLlm = std::make_shared<OpenAiCompat>(candidate.openai);
        newEmbedding = std::make_shared<OpenAiEmbedding>(candidate.embedding);
        newSummary = std::make_shared<SummaryManager>(800, newLlm);
        newBuilder = std::make_shared<ContextBuilder>(candidate.contextBuilder, newSummary);
        newBuilder->setColdStartSource(&achieve_);  // 冷启动数据源随 builder 重建
    } catch (const std::exception& e) {
        log::warn("Runtime", std::string("组件构造异常，保持原配置: ") + e.what());
        return false;
    }

    // 3) 原子切换服务指针
    {
        std::unique_lock<std::shared_mutex> lock(servicesMtx_);
        llm_ = newLlm;
        embedding_ = newEmbedding;
        summary_ = newSummary;
        builder_ = newBuilder;
        if (!candidate.botName.empty()) persona_.botName = candidate.botName;
        if (!candidate.character.empty()) persona_.character = candidate.character;
        if (!candidate.systemPromptPrefix.empty())
            persona_.systemPromptPrefix = candidate.systemPromptPrefix;
        if (!candidate.systemPromptNotice.empty())
            persona_.systemPromptNotice = candidate.systemPromptNotice;
        systemPrompt_ = rebuildSystemPrompt();
    }

    // 4) 刷新长驻组件配置与依赖
    registry_.clearDynamicTools();
    registerMemoryTools();
    FusionConfig fusionCfg = candidate.fusion;
    // 冷启动预算跟随 contextBuilder（不把两个开关同时暴露给配置）
    fusionCfg.coldStartRawTokens = candidate.contextBuilder.coldStartRawTokens;
    fusionCfg.coldStartMaxMessages = candidate.contextBuilder.coldStartMaxMessages;
    router_.update(fusionCfg, newSummary, newEmbedding);
    memory_.update(candidate.memory, newEmbedding);
    // 后端可能随配置切换：重建 provider（无线程/无网络，析构即回收；顺序先新后旧）
    {
        auto newProvider = makeMemoryProvider(candidate.memory.backend, memory_,
                                              candidate.memory);
        if (newProvider && !newProvider->available()) {
            log::warn("Runtime", "经历记忆后端不可用，降级为无长期召回运行: " +
                                     newProvider->name());
        }
        memoryProvider_ = std::move(newProvider);
    }
    inputBuffers_.updateConfig(candidate.inputBuffer);
    // 热更新只影响未触发的计时器；已开始的摘要任务继续运行
    lifecycle_->configure(candidate.conversation);

    {
        std::lock_guard<std::mutex> slock(stateMtx_);
        state_.botName = persona_.botName;
    }

    // 5) 发布新配置快照（此时依赖已就绪）
    configMgr_->set(std::make_shared<const AppConfig>(candidate));

    const std::int64_t now = std::time(nullptr);
    eventLog_.append({0, EventKind::ConfigReloaded, now, "配置热重载: " + configPath.string()});
    log::info("Runtime", "配置热重载成功: " + configPath.string());
    return true;
}

std::string Runtime::systemPrompt() const {
    std::shared_lock<std::shared_mutex> lock(servicesMtx_);
    return systemPrompt_;
}

std::shared_ptr<const AppConfig> Runtime::config() const {
    return configMgr_ ? configMgr_->get() : nullptr;
}

std::shared_ptr<ConfigManager> Runtime::configManager() const {
    return configMgr_;
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

    // remember / set_nickname / set_notes / recall_memory 由子任务 D 的
    // registerLegacyCompatTools 统一注册（走同一校验层，历史旧入口不得绕过新规则）。
    // 这里不再内联实现，避免出现可绕过权限校验的第二条路径。

    // set_public —— 模型只能把当前上下文【收窄】为私密。
    // 文档：记忆默认 conversation 可见；摘要输出与普通工具都不得扩大可见性
    // （T09：旧工具扩大可见性必须保持原可见范围）。
    ToolDef publicDef;
    publicDef.name = "set_public";
    publicDef.description =
        "在会话内容转为私密时调用，将当前上下文标记为私密（收窄）。不允许通过本工具扩大可见性。";
    publicDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"is_public", {{"type", "boolean"},
                          {"description", "true=公开（当前版本会被拒绝），false=私密"}}},
          {"reason", {{"type", "string"},
                      {"description", "设置公开/私密的原因（调试用）"}}}}},
        {"required", nlohmann::json::array({"is_public"})}};
    registry_.add(std::move(publicDef),
                  [this](const nlohmann::json& args) -> std::string {
                      if (!g_activeUnit) {
                          return ToolResult::failure(ErrorCode::StorageUnavailable,
                                                     "无当前上下文")
                              .toJsonString();
                      }
                      bool requestedPublic = false;
                      if (args.contains("is_public")) {
                          const auto& v = args["is_public"];
                          if (v.is_boolean()) {
                              requestedPublic = v.get<bool>();
                          } else if (v.is_string()) {
                              const std::string s = v.get<std::string>();
                              requestedPublic = (s == "true" || s == "1" || s == "公开");
                          } else {
                              return ToolResult::failure(ErrorCode::InvalidArgument,
                                                         "is_public 类型非法")
                                  .toJsonString();
                          }
                      }
                      // 扩大可见性只能由管理员通道/未来人工审核触发，模型工具一律拒绝
                      if (requestedPublic && !g_accessCtx.adminChannel) {
                          log::warn("Runtime",
                                    "set_public 拒绝扩大可见性: conv=" +
                                        g_activeConv.toString());
                          return ToolResult::failure(
                                     ErrorCode::NotFoundOrForbidden,
                                     "不允许扩大可见性；只能收窄为私密")
                              .toJsonString();
                      }
                      const std::string reason = args.value("reason", "");
                      log::info("Runtime",
                                "set_public 调用: conv=" + g_activeConv.toString() +
                                    " is_public=" + (requestedPublic ? "true" : "false") +
                                    " reason=" + reason +
                                    " topic=" + g_activeUnit->topic() +
                                    " isPublic=" +
                                    (g_activeUnit->isPublic() ? "true" : "false"));
                      FusionUnit* target = router_.setPublic(g_activeConv, requestedPublic);
                      if (!target) {
                          return ToolResult::failure(ErrorCode::StorageUnavailable,
                                                     "无法定位当前上下文")
                              .toJsonString();
                      }
                      g_activeUnit = target;
                      return ToolResult::success(
                                 nlohmann::json{{"visibility", requestedPublic ? "public"
                                                                              : "conversation"}})
                          .toJsonString();
                  },
                  ToolLayer::Builtin);

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

    // recall_memory 由子任务 D 的安全兼容别名提供（同一校验层 + AccessContext 过滤）

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
                          // 昵称重名必须 fail-closed（不得"子串匹配取第一个"而绑错人）：
                          // 以内部 ID 定位，按平台 ID 匹配优先，其次唯一昵称。
                          const PersonNode* p = graph_.findByPlatform(
                              g_activePlatform, queryName);
                          if (p == nullptr) {
                              std::string internalId;
                              if (!graph_.findByNameUnique(queryName, &internalId)) {
                                  const auto matches = graph_.findByNameAll(queryName);
                                  if (matches.empty()) {
                                      return ToolResult::failure(
                                                 ErrorCode::NotFoundOrForbidden,
                                                 "未找到唯一匹配的人（重名或不存在）")
                                          .toJsonString();
                                  }
                                  return ToolResult::failure(
                                             ErrorCode::Conflict,
                                             "昵称存在歧义（多个匹配），请用平台 ID 精确查询")
                                      .toJsonString();
                              }
                              p = graph_.findById(internalId);
                          }
                          if (p == nullptr) {
                              return ToolResult::failure(ErrorCode::NotFoundOrForbidden,
                                                         "未找到该人")
                                  .toJsonString();
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
        "当你阅读了用户的话后，内心感到被冒犯不想回复、想要已读不回、或者认为此时保持沉默更符合你的心情/不想回答时，调用此工具保持沉默。调用后你不会向用户发送任何消息。";
    keepSilentDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"reason",
           {{"type", "string"},
            {"description", "决定保持沉默/不想回答的内心理由（如：被冒犯了不想理会/无聊不想回答等）"}}}}},
        {"required", nlohmann::json::array({"reason"})}};
    keepSilentDef.isTerminal = true;
    registry_.add(
        std::move(keepSilentDef),
        [](const nlohmann::json& args) -> std::string {
            g_activeKeepSilent = true;
            std::string reason = args.value("reason", "");
            log::info("Runtime", "模型调用 keepsilent 保持沉默: " + reason);
            return "已选择保持沉默。本轮不会向用户发送任何回复。";
        },
        ToolLayer::Builtin);

    // keep_silent 别名兼容
    ToolDef keepSilentAliasDef;
    keepSilentAliasDef.name = "keep_silent";
    keepSilentAliasDef.description = "同 keepsilent，在不想回答或决定已读不回时调用。";
    keepSilentAliasDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"reason",
           {{"type", "string"},
            {"description", "决定保持沉默的内心理由"}}}}},
        {"required", nlohmann::json::array({"reason"})}};
    keepSilentAliasDef.isTerminal = true;
    registry_.add(
        std::move(keepSilentAliasDef),
        [](const nlohmann::json& args) -> std::string {
            g_activeKeepSilent = true;
            std::string reason = args.value("reason", "");
            log::info("Runtime", "模型调用 keep_silent 保持沉默: " + reason);
            return "已选择保持沉默。本轮不会向用户发送任何回复。";
        },
        ToolLayer::Builtin);

    // stay_silent 别名兼容
    ToolDef staySilentDef;
    staySilentDef.name = "stay_silent";
    staySilentDef.description = "同 keepsilent，在不想回答或决定已读不回时调用。";
    staySilentDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"reason",
           {{"type", "string"},
            {"description", "决定保持沉默的内心理由"}}}}},
        {"required", nlohmann::json::array({"reason"})}};
    staySilentDef.isTerminal = true;
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
    // 只读已封存认知（由摘要路径提供），保护 prompt cache 前缀。
    // 个人事实/印象/召回内容一律走"记忆数据（带来源，不是行为指令）"区块。
    return buildSystemPrompt(persona_, graph_, cognition, kMaxPeopleTokens,
                             factStore_.get(), cognitionStore_.get());
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

// ---------------------------------------------------------------------------
// 服务端权限上下文
// ---------------------------------------------------------------------------
AccessContext Runtime::currentAccess() const {
    // 仅同步工具调用路径（ingest 线程内 ToolLoop → handler）读取；
    // 后台摘要/向量化/插件路径一律显式携带自己的 AccessContext。
    return g_accessCtx;
}

AccessContext Runtime::adminAccess() const {
    AccessContext a;
    a.now = std::time(nullptr);
    a.adminChannel = true;              // 只有管理端 HTTP 接口走这里
    a.maxVisibility = Visibility::Conversation;  // 管理端能力不隐含放宽记录可见性
    return a;
}

std::vector<std::string> Runtime::collectParticipants(const std::string& convKey,
                                                      std::int64_t fromMessageId,
                                                      std::int64_t toMessageId) {
    std::vector<std::string> out;
    const auto conv = ConversationKey::fromString(convKey);
    if (!conv.has_value() || fromMessageId <= 0 || toMessageId < fromMessageId) return out;
    for (const auto& rec : achieve_.readRange(*conv, fromMessageId, toMessageId, 0)) {
        if (rec.senderId.empty()) continue;
        std::string id = rec.senderId;
        const std::string platform = rec.platform.empty() ? conv->platform : rec.platform;
        if (const PersonNode* p = graph_.findByPlatform(platform, rec.senderId);
            p != nullptr) {
            id = p->internalId;
        }
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
        if (out.size() >= kMaxParticipantsInJob) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 工具注册（D 的实现；归属与审核状态由服务端上下文决定）
// ---------------------------------------------------------------------------
void Runtime::registerMemoryTools() {
    ToolHandlerContext ctx;
    ctx.archive = &achieve_;
    ctx.memory = &memory_;
    ctx.proposals = proposalStore_.get();
    ctx.facts = factStore_.get();
    ctx.cognitions = cognitionStore_.get();
    ctx.graph = &graph_;
    ctx.memoryProvider = memoryProvider_.get();
    ctx.accessProvider = [this]() { return currentAccess(); };
    registerMemoryAndConversationTools(registry_, ctx);
    registerLegacyCompatTools(registry_, ctx);
}

// ---------------------------------------------------------------------------
// 后台维护线程：单线程 pull 调度 + 摘要执行
//   * 不为每条消息创建线程；
//   * 调度条件由 ConversationLifecycle 判定（静默到期 / 有未总结消息 /
//     满足阈值 / 无待处理输入 / 无生成请求 / 无在飞摘要）；
//   * 摘要范围在 tryBeginSummary 时冻结，成功只推进到冻结终点。
// ---------------------------------------------------------------------------
void Runtime::startMaintenance() {
    stopping_.store(false);
    maintenance_ = std::thread([this]() { maintenanceLoop(); });
}

void Runtime::stopMaintenance() {
    if (!maintenance_.joinable()) return;
    stopping_.store(true);
    maintenance_.join();
}

void Runtime::maintenanceLoop() {
    int tickCount = 0;
    while (!stopping_.load()) {
        const std::int64_t now = std::time(nullptr);
        try {
            lifecycleTick(now);
        } catch (const std::exception& e) {
            log::warn("Runtime", std::string("维护循环异常（继续运行）: ") + e.what());
        }
        // 向量化待办低频重试（失败不阻塞主对话）
        if (++tickCount % 60 == 0) {
            try {
                memory_.retryPendingEmbeddings(4, now);
            } catch (const std::exception& e) {
                log::warn("Runtime", std::string("向量化重试异常: ") + e.what());
            }
            // 提案待投递（同事务入 outbox；投递成功才标记 delivered，失败保留重试）
            if (proposalStore_) {
                try {
                    memory_.deliverPendingProposals(*proposalStore_, 32);
                } catch (const std::exception& e) {
                    log::warn("Runtime", std::string("提案投递异常: ") + e.what());
                }
            }
            lifecycle_->persist();
        }
        // 1s 粒度：静默判定本身是秒级，重试额度落盘独立于每条消息
        for (int i = 0; i < 10 && !stopping_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    try {
        // 关闭前尽力排空：待投递提案（outbox 已持久化，失败下次启动继续）
        if (proposalStore_) memory_.deliverPendingProposals(*proposalStore_, 64);
    } catch (...) {
    }
    try {
        lifecycle_->persist();
    } catch (...) {
    }
}

void Runtime::lifecycleTick(std::int64_t now) {
    if (!lifecycle_) return;
    std::vector<std::string> keys;
    try {
        keys = achieve_.conversationKeys();  // 启动/重启后覆盖未提交范围
    } catch (const std::exception& e) {
        log::warn("Runtime", std::string("档案会话枚举失败: ") + e.what());
        return;
    }
    for (const auto& key : keys) {
        if (stopping_.load()) return;
        const auto conv = ConversationKey::fromString(key);
        if (!conv.has_value()) continue;
        std::optional<SummaryJob> job = lifecycle_->tryBeginSummary(*conv, now);
        if (!job.has_value()) continue;  // 不满足等待条件（含已有在飞任务）
        runSummaryJob(*job, now);
    }
}

bool Runtime::runSummaryJob(const SummaryJob& job, std::int64_t now) {
    std::shared_ptr<SummaryManager> summary;
    std::shared_ptr<ContextBuilder> builder;
    {
        std::shared_lock<std::shared_mutex> lock(servicesMtx_);
        summary = summary_;
    }
    if (!summary) {
        lifecycle_->failSummary(job.jobId, "summary service unavailable", now);
        return false;
    }

    // 1) 读取冻结范围（值拷贝快照；Achieve 内部持锁期间不调用外部组件）
    const auto conv = ConversationKey::fromString(job.conversationKey);
    std::vector<ArchiveRecord> records;
    if (conv.has_value()) {
        records = achieve_.readRange(*conv, job.fromMessageId, job.toMessageId, 0);
    }

    // 2) 没有可总结内容：可审计处理结果 + 推进游标，不产生无限重复任务
    if (records.empty()) {
        memory_.markRangeProcessed(job.conversationKey, job.toMessageId,
                                   "empty_range", now);
        lifecycle_->completeSummary(job.jobId, job.toMessageId, now);
        std::lock_guard<std::mutex> lock(statsMtx_);
        ++summaryStats_.skippedNoContent;
        return true;
    }

    // 3) 构造摘要请求：材料已剔除 reasoning / 内部工具参数（档案投影保证）
    SummaryRequest req;
    req.kind = SummaryKind::EpisodicMemory;
    req.conversationKey = job.conversationKey;
    req.participants = job.participants.empty()
                           ? collectParticipants(job.conversationKey,
                                                 job.fromMessageId, job.toMessageId)
                           : job.participants;
    req.fromMessageId = job.fromMessageId;
    req.toMessageId = job.toMessageId;
    req.visibility = Visibility::Conversation;  // 默认会话可见，只可收窄
    req.source = "silence_summary";
    req.now = now;
    req.material.reserve(records.size());
    for (const auto& rec : records) {
        Msg m;
        m.role = rec.role;
        m.text = rec.text;
        m.createdAt = rec.createdAt;
        m.senderId = rec.senderId;
        m.senderName = rec.senderName;
        m.platform = rec.platform;
        m.groupId = rec.groupId;
        // 绝不把 reasoning 带进摘要材料
        m.reasoningContent.clear();
        req.material.push_back(std::move(m));
    }

    // 4) 生成（失败不得当成成功）
    SummaryOutcome out;
    try {
        out = summary->summarize(req);
    } catch (const std::exception& e) {
        lifecycle_->failSummary(job.jobId, std::string("summary exception: ") + e.what(), now);
        std::lock_guard<std::mutex> lock(statsMtx_);
        ++summaryStats_.failed;
        return false;
    }
    if (!out.ok) {
        lifecycle_->failSummary(job.jobId,
                                out.error.empty() ? "summary failed" : out.error, now);
        std::lock_guard<std::mutex> lock(statsMtx_);
        ++summaryStats_.failed;
        return false;
    }

    // 5) 没有值得保留的内容：可审计结果 + 推进游标（不是失败，不重试）
    if (out.text.empty()) {
        memory_.markRangeProcessed(job.conversationKey, job.toMessageId,
                                   "no_retainable_content", now);
        lifecycle_->completeSummary(job.jobId, job.toMessageId, now);
        std::lock_guard<std::mutex> lock(statsMtx_);
        ++summaryStats_.skippedNoContent;
        return true;
    }

    // 6) 原子写入摘要 + 游标 + 向量化待办 + 提案待投递（同一 SQLite 事务，B 保证）。
    //    摘要同步产出的事实/关系提案先入 proposal_outbox（同事务），
    //    再由后台 tick 投递给 ProposalStore —— 避免"写库成功→投递"之间崩溃丢提案。
    std::vector<Proposal> proposals;
    proposals.reserve(out.factProposals.size() + out.relationshipProposals.size());
    for (auto proposal : out.factProposals) {
        proposal.conversationKey = job.conversationKey;
        proposal.source = "silence_summary";
        proposal.visibility = Visibility::Conversation;
        proposal.createdAt = now;
        proposal.validFrom = now;
        proposals.push_back(std::move(proposal));
    }
    for (auto proposal : out.relationshipProposals) {
        proposal.conversationKey = job.conversationKey;
        proposal.source = "silence_summary";
        proposal.visibility = Visibility::Conversation;
        proposal.createdAt = now;
        proposal.validFrom = now;
        proposals.push_back(std::move(proposal));
    }

    SummaryRecord rec;
    rec.conversationKey = job.conversationKey;
    rec.participants = req.participants;
    rec.kind = SummaryKind::EpisodicMemory;
    rec.summary = out.text;
    rec.visibility = Visibility::Conversation;
    rec.createdAt = now;
    rec.eventTime = records.back().createdAt != 0 ? records.back().createdAt : now;
    rec.fromMessageId = job.fromMessageId;
    rec.toMessageId = job.toMessageId;
    rec.source = "silence_summary";
    const SummaryWriteResult writeResult = memory_.writeSummary(rec, proposals);
    if (!writeResult.ok) {
        lifecycle_->failSummary(job.jobId,
                                writeResult.message.empty() ? "summary write failed"
                                                            : writeResult.message,
                                now);
        std::lock_guard<std::mutex> lock(statsMtx_);
        ++summaryStats_.failed;
        return false;
    }
    eventLog_.append({0, EventKind::MemoryRemembered, now,
                      "经历摘要已提交: " + job.conversationKey + " [" +
                          std::to_string(job.fromMessageId) + "," +
                          std::to_string(job.toMessageId) + "] id=" +
                          std::to_string(writeResult.memoryId)});

    lifecycle_->completeSummary(job.jobId, job.toMessageId, now);
    {
        std::lock_guard<std::mutex> lock(statsMtx_);
        ++summaryStats_.succeeded;
    }
    log::info("Runtime", "经历摘要完成: " + job.conversationKey + " id=" +
                             std::to_string(writeResult.memoryId) +
                             " embedding=" + toString(writeResult.embeddingStatus));
    return true;
}

// ---------------------------------------------------------------------------
// 管理端只读观测
// ---------------------------------------------------------------------------
std::vector<Proposal> Runtime::pendingProposals(std::size_t limit) const {
    if (!proposalStore_) return {};
    return proposalStore_->listPending(adminAccess(), limit);
}

std::vector<ConversationLifecycleState> Runtime::lifecycleStates() const {
    if (!lifecycle_) return {};
    return lifecycle_->snapshots();
}

bool Runtime::clearLifecycleFailure(const std::string& conversationKey) {
    if (!lifecycle_) return false;
    const auto conv = ConversationKey::fromString(conversationKey);
    if (!conv.has_value()) return false;
    const bool ok = lifecycle_->clearFailure(*conv);
    if (ok) lifecycle_->persist();
    return ok;
}

Runtime::SummaryStats Runtime::summaryStats() const {
    std::lock_guard<std::mutex> lock(statsMtx_);
    return summaryStats_;
}

// ---------------------------------------------------------------------------
// ingest：接收 → 防抖 → 路由 → 构建 → 工具循环
//   * 每条有效消息（含 follower）更新活动版本与活动时间；
//   * 生成开始/结束成对上报（RAII），保证不会永久停调度；
//   * 所有档案写入走 appendChecked，只有确认落盘的消息才进入总结范围。
// ---------------------------------------------------------------------------
BotReply Runtime::ingest(IncomingMessage message) {
    const std::int64_t now = std::time(nullptr);
    g_activeKeepSilent = false;

    // 捕获当前代际的服务快照（RCU 模式），确保单次调用内部的组件生命周期一致
    std::shared_ptr<Llm> currentLlm;
    std::shared_ptr<ContextBuilder> currentBuilder;
    std::shared_ptr<SummaryManager> currentSummary;
    {
        std::shared_lock<std::shared_mutex> lock(servicesMtx_);
        currentLlm = llm_;
        currentBuilder = builder_;
        currentSummary = summary_;
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

    // 有效消息（含 follower）都算会话活动：更新活动版本并取消旧静默判定
    if (lifecycle_) {
        lifecycle_->noteIncomingActivity(message.conversation, now);
        lifecycle_->noteInputBuffered(message.conversation, now);
    }
    if (pushRes == PushResult::AcceptedFollower) {
        // 作为跟随消息已合流进当前物理会话等待批次，静默返回，由 Leader 统一处理
        return BotReply{message.conversation, ""};
    }

    // 当前线程为该物理会话的 Leader，等待防抖窗口结束，获取当前批次所有消息
    BufferedBatch batch = buffer->waitForBatch();
    if (lifecycle_) lifecycle_->noteInputConsumed(batch.conversation, now);
    if (batch.empty()) {
        return BotReply{message.conversation, ""};
    }

    // 2) 设置当前线程的会话上下文与【服务端权限上下文】
    //    （仅同步工具调用路径使用；后台任务显式携带自己的归属）
    const auto& targetMsg = batch.rawMessages.back();
    g_activeConv = targetMsg.conversation;
    g_activeSenderId = targetMsg.senderId;
    g_activeSenderName = targetMsg.senderName;
    g_activePlatform = targetMsg.platform;
    g_activeGroupId = targetMsg.groupId;
    {
        AccessContext ctx;
        ctx.conversationKey = targetMsg.conversation.toString();
        ctx.scope = targetMsg.conversation.scope;
        ctx.platform = targetMsg.platform;
        ctx.platformUserId = targetMsg.senderId;
        ctx.groupId = targetMsg.groupId;
        ctx.adminChannel = false;  // 模型工具调用永远不是管理员通道
        ctx.maxVisibility = Visibility::Conversation;
        ctx.allowCrossConversation = false;  // 当前阶段历史查询仅限当前物理会话
        ctx.now = now;
        if (targetMsg.conversation.scope == ConversationScope::Private) {
            if (const PersonNode* p = graph_.findByPlatform(targetMsg.platform,
                                                            targetMsg.senderId);
                p != nullptr) {
                ctx.requesterPersonId = p->internalId;
                ctx.participants.push_back(p->internalId);
            }
        } else {
            for (const auto& raw : batch.rawMessages) {
                if (const PersonNode* p = graph_.findByPlatform(raw.platform, raw.senderId);
                    p != nullptr) {
                    if (std::find(ctx.participants.begin(), ctx.participants.end(),
                                  p->internalId) == ctx.participants.end()) {
                        ctx.participants.push_back(p->internalId);
                    }
                }
            }
        }
        g_activeMemoryPerson = ctx.requesterPersonId;
        g_accessCtx = std::move(ctx);
    }

    struct ContextGuard {
        ~ContextGuard() {
            g_activeUnit = nullptr;
            g_activeConv = {};
            g_activeMemoryPerson.clear();
            g_activeSenderId.clear();
            g_activeSenderName.clear();
            g_activePlatform.clear();
            g_activeGroupId.clear();
            g_accessCtx = {};
        }
    } contextGuard;

    // 3) 路由与关系图谱登记：批次内每条原始消息进行 route 处理
    FusionUnit* unit = nullptr;
    for (const auto& raw : batch.rawMessages) {
        unit = router_.route(raw, now);
    }
    if (!unit) {
        return BotReply{batch.conversation, ""};
    }
    g_activeUnit = unit;

    // 生成期间不得并发读取半成品对话：开始/结束必须成对（RAII）
    struct GenerationGuard {
        ConversationLifecycle* lc;
        ConversationKey conv;
        ~GenerationGuard() {
            if (lc) lc->noteGenerationEnd(conv);
        }
    } generationGuard{lifecycle_.get(), batch.conversation};
    if (lifecycle_) lifecycle_->noteGenerationStart(batch.conversation, now);

    ChatResponse resp;
    bool reColdStarted = false;
    {
        // 上下文级锁：同 unit 串行（含 LLM 往返），跨 unit 并行
        std::lock_guard<std::mutex> lock(unit->mtx);

        // 4) 批次内所有原始消息分别落入物理会话的真实档案。
        //    只有确认落盘成功的消息才进入总结范围（appendChecked）。
        for (const auto& raw : batch.rawMessages) {
            Msg rawTurn{Role::User};
            rawTurn.text = raw.text;
            rawTurn.createdAt = now;
            rawTurn.senderId = raw.senderId;
            rawTurn.senderName = router_.displayName(raw);
            rawTurn.platform = raw.platform;
            rawTurn.groupId = raw.groupId;
            const AppendResult ar = achieve_.appendChecked(batch.conversation, rawTurn);
            if (!ar.ok) {
                log::warn("Runtime", "用户消息落档案失败（不进入总结范围）: " + ar.error);
            }
        }

        // 5) 批次内重组好的独立上下文 turns 追加进 unit 上下文
        for (auto& turn : batch.turns) {
            unit->append(std::move(turn));
        }

        // 6) 构建请求（当前回合已 append 进 unit；上下文不够 → 重新冷启动压缩）
        BuildInput in;
        in.systemPrompt = systemPrompt_;
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
                               const AppendResult ar = achieve_.appendChecked(conv, m);
                               if (!ar.ok) {
                                   log::warn("Runtime",
                                             "助手/工具消息落档案失败: " + ar.error);
                               }
                               unit->append(m);
                           });
        unit->recordUsage(resp.usage);
    }
    // 8) 锁外：状态快照 + 审计 + Facts 重建
    for (const auto& raw : batch.rawMessages) {
        noteMessage(raw.conversation, raw.senderId);
    }
    if (reColdStarted) {
        // 上下文压缩只产出 context_compaction：不写经历、不推进静默游标、
        // 不提升可见性 —— 经历写入只发生在后台静默摘要路径。
        eventLog_.append({0, EventKind::SummaryApplied, now, "上下文重新冷启动"});
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
