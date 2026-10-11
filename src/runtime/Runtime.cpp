#include "runtime/Runtime.h"
#include "runtime/ContactTools.h"
#include "runtime/MessageTools.h"

#include "admin/AdminServer.h"
#include "config/AppConfig.h"
#include "config/ConfigManager.h"
#include "config/embedding/EmbeddingConfig.h"
#include "config/openai/OpenaiConfig.h"
#include "providers/llm/openai/OpenAiCompat.h"
#include "providers/embedding/openai/OpenAiEmbedding.h"
#include "providers/llm/tool/ToolLoop.h"
#include "providers/mcp/McpManager.h"
#include "core/message/History.h"
#include "log/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mio {
namespace {

constexpr int kMaxPeopleTokens = 1200;

// 工具（set_public / update_topic / get_current_user_qq 等）需要定位"当前正在处理"的 unit 与其来源；
// 并发 ingest 各自线程互不干扰，故用 thread_local
static thread_local FusionUnit* g_activeUnit = nullptr;
static thread_local ConversationKey g_activeConv;
static thread_local std::string g_activeSenderId;
static thread_local std::string g_activeSenderName;
static thread_local std::string g_activePlatform;
static thread_local std::string g_activeGroupId;
static thread_local bool g_activeKeepSilent = false;
static thread_local std::vector<std::string> g_replyableMessageIds;
// 维护回合标记：置位期间禁止向平台发送消息（send_message 会被拒绝），
// 也不会写进真实会话档案 —— 维护回合只能调记忆写入一类的副作用工具。
static thread_local bool g_maintenanceMode = false;

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

// prompt 指纹（FNV-1a 64bit → 16 位十六进制）：只用于在日志里判断"前缀是否
// 真的逐字节变了"，不做任何安全用途。
std::string promptFingerprint(const std::string& prompt) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : prompt) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    static const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[hash & 0xF];
        hash >>= 4;
    }
    return out;
}

// 当天本地 00:00（epoch 秒）：维护任务用它筛"今天有活动"的会话。
// tm_isdst = -1 交给 mktime 判定，跨 DST 的日期才不会差一小时。
std::int64_t localDayStart(std::int64_t now) {
    const std::time_t t = static_cast<std::time_t>(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&tm));
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
      router_{graph_, achieve_, summary_, configMgr_->get()->fusion, embedding_},
      inputBuffers_{configMgr_->get()->inputBuffer} {
    log::initFromEnvironment();  // MIO_LOG_LEVEL，重复调用安全

    if (configMgr_ && configMgr_->get()) {
        auto cfg = configMgr_->get();
        if (!cfg->botName.empty() && botName == "Mio") persona_.botName = cfg->botName;
        if (!cfg->character.empty()) persona_.character = cfg->character;
        if (!cfg->systemPromptPrefix.empty()) persona_.systemPromptPrefix = cfg->systemPromptPrefix;
        if (!cfg->systemPromptNotice.empty()) persona_.systemPromptNotice = cfg->systemPromptNotice;
        splitterConfig_ = cfg->splitter;
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

    // 长期记忆已交给 MCP（如 graphiti_*），这里不再有"摘要 → 记忆写入"的咽喉点
    refreshFactsNow("startup");
    state_.botName = persona_.botName;
    state_.messageCount = 0;

    int adminPort = (configMgr_ && configMgr_->get()) ? configMgr_->get()->adminPort : 6188;
    if (adminPort > 0) {
        adminServer_ = std::make_unique<AdminServer>(*this, adminPort);
        adminServer_->start();
    }

    // 定时事项最后启动：此时图谱/日记/服务指针/工具表都已就位
    if (configMgr_ && configMgr_->get()) startScheduler(configMgr_->get()->maintenance);
}

Runtime::Runtime(std::string botName, std::filesystem::path dataDir,
                 std::unique_ptr<Llm> llm)
    : Runtime(std::move(botName), std::move(dataDir),
              std::shared_ptr<Llm>(std::move(llm))) {}

Runtime::~Runtime() {
    // 先停定时线程：它回调进 Runtime（刷新 Facts / 维护回合），必须在
    // 图谱/日记/服务指针析构之前退出
    if (scheduler_) {
        scheduler_->stop();
        scheduler_.reset();
    }
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
        auto newBuilder = std::make_shared<ContextBuilder>(newConfig->contextBuilder, newSummary);

        // 重载时清理动态工具并重新热加载 MCP 服务
        registry_.clearDynamicTools();
        if (mcpManager_) {
            mcpManager_->reload(newConfig->mcp, registry_);
        }

        // 原子切换服务指针；提示词重建放到锁外（refreshFactsNow 自己取锁，
        // 且它是"缓存前缀优先"策略里的一个显式刷新边界）
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
        }
        refreshFactsNow("config-reload");

        // 刷新长驻组件配置与依赖
        router_.update(newConfig->fusion, newSummary, newEmbedding);
        inputBuffers_.updateConfig(newConfig->inputBuffer);

        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            state_.botName = persona_.botName;
        }
        {
            std::lock_guard<std::mutex> lock(senderMtx_);
            splitterConfig_ = newConfig->splitter;
        }

        const std::int64_t now = std::time(nullptr);
        eventLog_.append({0, EventKind::ConfigReloaded, now, "配置热重载: " + configPath.string()});
        // 定时事项按新配置重建（停旧线程 → 按新任务表起新线程）
        startScheduler(newConfig->maintenance);
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

void Runtime::setMessageSender(MessageSender sender) {
    std::lock_guard<std::mutex> lock(senderMtx_);
    if (!sender) { messageSender_ = {}; return; }
    messageSender_ = [sender = std::move(sender)](const ConversationKey& conv, const OutgoingMessage& message) {
        // Older adapters cannot represent metadata; never silently drop it.
        if (!message.mentionUserIds.empty() || !message.replyToMessageId.empty()) return false;
        return sender(conv, conv.scope == ConversationScope::Private ? conv.id : std::string{}, message.text);
    };
}

void Runtime::setStructuredMessageSender(StructuredSender sender) {
    std::lock_guard<std::mutex> lock(senderMtx_);
    messageSender_ = std::move(sender);
}

bool Runtime::hasMessageSender() const {
    std::lock_guard<std::mutex> lock(senderMtx_);
    return messageSender_ != nullptr;
}

namespace {

std::vector<std::string> splitByDelimiter(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> tokens;
    if (delimiter.empty()) {
        std::string s = str;
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        if (!s.empty()) tokens.push_back(std::move(s));
        return tokens;
    }
    std::size_t start = 0;
    std::size_t pos = 0;
    while ((pos = str.find(delimiter, start)) != std::string::npos) {
        std::string token = str.substr(start, pos - start);
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), token.end());
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
        start = pos + delimiter.length();
    }
    if (start < str.length()) {
        std::string token = str.substr(start);
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), token.end());
        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }
    }
    return tokens;
}

int calculateDelayMs(const std::string& previousText, const MessageSplitterConfig& cfg) {
    if (!cfg.enabled) return 0;
    std::size_t charCount = 0;
    for (std::size_t i = 0; i < previousText.size(); ) {
        unsigned char c = static_cast<unsigned char>(previousText[i]);
        if (c < 0x80) i += 1;
        else if ((c >> 5) == 0x6) i += 2;
        else if ((c >> 4) == 0xE) i += 3;
        else if ((c >> 3) == 0x1E) i += 4;
        else i += 1;
        ++charCount;
    }
    int delay = cfg.baseDelayMs + static_cast<int>(charCount) * cfg.delayPerCharMs;
    if (delay > cfg.maxDelayMs) delay = cfg.maxDelayMs;
    if (delay < 0) delay = 0;
    return delay;
}

// 分片睡眠的粒度：新消息打断时最多多等这么久，而不是等满整个打字延迟
constexpr int kSplitterSleepSliceMs = 50;

// 回复里是否真的存在可发送内容。
// 模型偶尔只回空白或一个分段符（"[SEP]"），拆分后气泡为空——那不是发送
// 失败，而是没有内容可发，必须按静默处理，否则会把整轮回复判为投递失败。
bool hasDeliverableText(const std::string& text, const MessageSplitterConfig& cfg) {
    const auto visible = [](const std::string& token) {
        return token.find_first_not_of(" \t\r\n") != std::string::npos;
    };
    if (cfg.enabled && !cfg.delimiter.empty()) {
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto pos = text.find(cfg.delimiter, start);
            const auto token = text.substr(
                start, pos == std::string::npos ? std::string::npos : pos - start);
            if (visible(token)) return true;
            if (pos == std::string::npos) break;
            start = pos + cfg.delimiter.size();
        }
        return false;
    }
    return visible(text);
}

} // namespace

bool Runtime::emit(const ConversationKey& conv, const std::string&, const std::string& rawText) {
    return emit(conv, OutgoingMessage{rawText, {}, {}});
}

bool Runtime::emit(const ConversationKey& conv, const OutgoingMessage& message) {
    const auto& rawText = message.text;
    if (rawText.empty()) return false;
    // 维护回合只能产生记忆/备注一类的副作用，绝不允许真的发消息出去
    if (g_maintenanceMode) {
        log::warn("Runtime", "维护回合中禁止发送平台消息，已拒绝: " + conv.toString());
        return false;
    }

    StructuredSender sender;
    MessageSplitterConfig splitter;
    {
        std::lock_guard<std::mutex> lock(senderMtx_);
        sender = messageSender_;
        splitter = splitterConfig_;
    }
    if (!sender) {
        log::warn("Runtime", "未配置 MessageSender，无法发送平台消息: " + rawText);
        return false;
    }

    std::vector<std::string> bubbles;
    if (splitter.enabled && !splitter.delimiter.empty() && rawText.find(splitter.delimiter) != std::string::npos) {
        bubbles = splitByDelimiter(rawText, splitter.delimiter);
    } else {
        std::string s = rawText;
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        if (!s.empty()) {
            bubbles.push_back(std::move(s));
        }
    }

    if (bubbles.empty()) return false;
    const auto generation = g_activeUnit ? g_activeUnit->generation() : 0;
    const auto stillCurrent = [generation] {
        return !g_activeUnit || g_activeUnit->generation() == generation;
    };
    for (std::size_t i = 0; i < bubbles.size(); ++i) {
        if (i > 0 && splitter.enabled) {
            int delayMs = calculateDelayMs(bubbles[i - 1], splitter);
            // 分片等待：被新消息打断时立刻停止连发，不再等满整个打字延迟
            for (int slept = 0; slept < delayMs; slept += kSplitterSleepSliceMs) {
                if (!stillCurrent()) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min(kSplitterSleepSliceMs, delayMs - slept)));
            }
        }
        if (!stillCurrent()) return false;
        OutgoingMessage bubble{bubbles[i], {}, {}};
        if (i == 0) {
            bubble.mentionUserIds = message.mentionUserIds;
            bubble.replyToMessageId = message.replyToMessageId;
        }
        if (!sender(conv, bubble)) return false;
    }
    return true;
}

void Runtime::registerBuiltinTools() {
    // 通讯录变更只置脏标记，**不**在这里重建 systemPrompt：
    // 它在 wire 请求的第一位，改一个字节就让所有会话的整段前缀缓存失效。
    // 真正的重建推迟到刷新边界（见 refreshFactsNow）。
    registerContactTools(registry_, graph_, [this] {
        std::unique_lock<std::shared_mutex> lock(servicesMtx_);
        factsDirty_ = true;
        log::info("Runtime",
                  "通讯录已更新：按缓存前缀优先策略，systemPrompt 推迟到下一个刷新边界重建");
    });
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
                      // 维护回合里的 g_activeUnit 是上下文副本，而本工具真正改的是
                      // Router 里那个真实 unit（按会话 key 定位）—— 维护回合不许改。
                      if (g_maintenanceMode)
                          return "error: 维护回合中不可修改会话公开性";
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
                      if (const auto p = graph_.platformContact(
                              g_activePlatform, g_activeSenderId); p) {
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

    // 通讯录查询已收敛到 list_contacts（见 ContactTools.cpp）：原先这里的
    // get_known_person_qq 与它功能重复、只是返回字段略少，两个都留着模型会
    // 随机挑一个，故删除。

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

    registerMessageTools(registry_, [] {
        return MessageToolContext{g_activeConv, g_activePlatform, g_replyableMessageIds};
    }, [this](const ConversationKey& conv, const OutgoingMessage& message) {
        return emit(conv, message);
    });

    // wait —— 模型自主等待指定时长后再继续思考与行动
    auto createWaitDef = []() {
        ToolDef d;
        d.name = "wait";
        d.description =
            "让系统等待/暂停指定的时长（秒），等待结束后会再次唤醒你继续思考和采取下一步行动。"
            "适用于需要稍作停顿再继续发言、给用户留出反应阅读时间、或者先发消息告知对方稍等后再进行查阅思考的场景。";
        d.parametersJsonSchema = {
            {"type", "object"},
            {"properties",
             {{"seconds",
               {{"type", "number"},
                {"description", "需要等待的秒数（支持整数或小数，例如 3 或 5.0，建议 1~30 秒，最大不超过 60 秒）"}}},
              {"duration",
               {{"type", "number"},
                {"description", "同 seconds，需要等待的秒数（备用字段）"}}},
              {"reason",
               {{"type", "string"},
                {"description", "等待的原因或打算下一步做的事情（可选）"}}}}},
            {"required", nlohmann::json::array({"seconds"})}};
        return d;
    };

    auto waitHandler = [](const nlohmann::json& args) -> std::string {
        double seconds = 0.0;
        auto parseSec = [&](const nlohmann::json& val) -> bool {
            if (val.is_number()) {
                seconds = val.get<double>();
                return true;
            }
            if (val.is_string()) {
                try {
                    seconds = std::stod(val.get<std::string>());
                    return true;
                } catch (...) {}
            }
            return false;
        };

        if (args.contains("seconds")) {
            parseSec(args["seconds"]);
        } else if (args.contains("duration")) {
            parseSec(args["duration"]);
        }

        if (seconds < 0.1) {
            seconds = 0.5;
        } else if (seconds > 60.0) {
            seconds = 60.0;
        }

        std::string reason = args.value("reason", "");
        log::info("Runtime",
                  "模型调用 wait: 等待 " + std::to_string(seconds) + " 秒" +
                      (reason.empty() ? "" : (", 理由=" + reason)));

        const int totalMs = static_cast<int>(seconds * 1000.0);
        constexpr int sliceMs = 100;
        int elapsedMs = 0;
        const std::uint64_t waitInitialGen = g_activeUnit ? g_activeUnit->generation() : 0;
        while (elapsedMs < totalMs) {
            if (g_activeUnit && g_activeUnit->generation() != waitInitialGen) {
                log::info("Runtime", "wait 过程中收到新消息打断，提前结束等待");
                break;
            }
            int curSlice = std::min(sliceMs, totalMs - elapsedMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(curSlice));
            elapsedMs += curSlice;
        }

        std::ostringstream oss;
        oss << "已等待 " << std::fixed << std::setprecision(1) << seconds
            << " 秒。等待已结束，当前时间：" << nowTimeString()
            << "。你可以继续思考并决定是否调用工具或向用户发送消息。";
        return oss.str();
    };

    ToolDef waitDef = createWaitDef();
    registry_.add(std::move(waitDef), waitHandler, ToolLayer::Builtin);

    ToolDef sleepDef = createWaitDef();
    sleepDef.name = "sleep";
    sleepDef.description = "同 wait，等待指定时长后再次继续思考和行动。";
    registry_.add(std::move(sleepDef), waitHandler, ToolLayer::Builtin);
}

std::string Runtime::rebuildSystemPrompt(
    const std::vector<DiaryEntry>& cognition) {
    // 只读已封存认知（由摘要路径提供），保护 prompt cache 前缀
    if (!cognition.empty()) cognition_ = cognition;
    return buildSystemPrompt(persona_, graph_, cognition_, kMaxPeopleTokens);
}

// 按"缓存前缀优先"策略，systemPrompt_ 只在明确的刷新边界重建。调用方应
// 传一个稳定的 reason 字符串（startup / config-reload / reColdStart / daily…），
// 便于在日志里对账"前缀到底什么时候被换了"。
void Runtime::refreshFactsNow(const std::string& reason,
                              std::vector<DiaryEntry> cognition) {
    std::unique_lock<std::shared_mutex> lock(servicesMtx_);
    const bool wasDirty = factsDirty_;
    systemPrompt_ = rebuildSystemPrompt(cognition);
    factsDirty_ = false;
    // fp 相同 = 前缀逐字节未变 = 缓存没被打断；相同 fp 的"重建"是零成本操作。
    log::info("Runtime", "systemPrompt 重建: reason=" + reason +
                             " dirty=" + (wasDirty ? "true" : "false") +
                             " bytes=" + std::to_string(systemPrompt_.size()) +
                             " fp=" + promptFingerprint(systemPrompt_));
}

// 定时任务接线：动作名 → Runtime 能力。调度器本身不认识业务。
void Runtime::startScheduler(const MaintenanceConfig& config) {
    if (scheduler_) {  // 配置热重载：先停旧线程再按新表重建
        scheduler_->stop();
        scheduler_.reset();
    }
    if (!config.enabled || config.tasks.empty()) {
        log::info("Runtime", "定时事项未启用");
        return;
    }
    scheduler_ = std::make_unique<Scheduler>(
        config.tasks, [this](const ScheduledTask& task) {
            if (task.action == "refresh_facts") {
                // 缓存前缀优先策略里唯一的常规刷新边界：消费日记 + 重建 Facts
                refreshFactsNow("scheduled:" + task.name, diary_.consume());
            } else if (task.action == "prompt") {
                runMaintenancePrompt(task.prompt);
            } else {
                log::warn("Runtime", "未知的定时任务动作: " + task.action +
                                          " (name=" + task.name + ")");
            }
        });
    scheduler_->start();
}

namespace {

// 维护回合的线程上下文：工具处理器读的是 thread_local，必须逐项就位。
// 同时把 g_maintenanceMode 置位，禁止对外发送。
struct MaintenanceTurn {
    explicit MaintenanceTurn(const FusionRouter::UnitSnapshot& target,
                             FusionUnit* unit) {
        g_maintenanceMode = true;
        g_activeUnit = unit;
        g_activeConv = target.conversation;
        g_activeSenderId = target.conversation.scope == ConversationScope::Private
                               ? target.conversation.id
                               : std::string{};
        g_activeSenderName.clear();
        g_activePlatform.clear();
        g_activeGroupId = target.conversation.scope == ConversationScope::Group
                              ? target.conversation.id
                              : std::string{};
        g_activeKeepSilent = false;
        g_replyableMessageIds.clear();
    }
    ~MaintenanceTurn() {
        g_maintenanceMode = false;
        g_activeUnit = nullptr;
        g_activeConv = {};
        g_activeSenderId.clear();
        g_activeSenderName.clear();
        g_activePlatform.clear();
        g_activeGroupId.clear();
        g_activeKeepSilent = false;
        g_replyableMessageIds.clear();
    }
    MaintenanceTurn(const MaintenanceTurn&) = delete;
    MaintenanceTurn& operator=(const MaintenanceTurn&) = delete;
};

} // namespace

void Runtime::runMaintenancePrompt(const std::string& prompt) {
    if (prompt.empty()) return;

    std::shared_ptr<Llm> llm;
    std::shared_ptr<ContextBuilder> builder;
    std::string systemPrompt;
    {
        std::shared_lock<std::shared_mutex> lock(servicesMtx_);
        llm = llm_;
        builder = builder_;
        systemPrompt = systemPrompt_;  // 维护回合用当前已渲染的 Facts（不额外换前缀）
    }
    if (!llm || !builder) return;

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const auto targets = router_.activeUnitsSince(localDayStart(now));
    if (targets.empty()) {
        log::info("Runtime", "维护回合跳过：今天没有活动会话");
        return;
    }
    log::info("Runtime", "维护回合开始: 目标会话数=" + std::to_string(targets.size()));

    for (const auto& target : targets) {
        // 在上下文**副本**上跑：维护回合不写回真实历史，冷启动压缩也只影响副本
        FusionUnit workspace(target.conversation, target.context);
        if (!target.topic.empty()) workspace.setTopic(target.topic);
        Msg ask{Role::User};
        ask.text = prompt;
        ask.createdAt = now;
        workspace.append(std::move(ask));

        ChatRequest req;
        try {
            BuildInput in;
            in.systemPrompt = systemPrompt;
            in.unit = &workspace;
            in.now = now;
            req = builder->build(in).request;
        } catch (const std::exception& error) {
            log::error("Runtime", "维护回合构建请求失败: " +
                                      target.conversation.toString() + " " +
                                      error.what());
            continue;
        }
        req.tools = registry_.defs();

        MaintenanceTurn turn(target, &workspace);
        ToolLoopOptions loopOpt;
        loopOpt.shouldInterrupt = [] { return false; };
        try {
            // sink 丢弃：维护回合的输出不进档案、不进 unit 上下文
            runToolLoop(*llm, registry_, req, loopOpt, [](const Msg&) {});
            log::info("Runtime", "维护回合完成: " + target.conversation.toString());
        } catch (const std::exception& error) {
            log::error("Runtime", "维护回合失败: " + target.conversation.toString() +
                                      " " + error.what());
        }
    }
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

BotReply Runtime::ingest(IncomingMessage message) {
    const std::int64_t now = std::time(nullptr);

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
    message.atUserIds = FusionRouter::mentionedUserIds(message);
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

    struct ProcessingGuard {
        std::shared_ptr<InputBuffer> buf;
        ~ProcessingGuard() {
            if (buf) buf->decrementProcessing();
        }
    } procGuard{buffer};
    buffer->incrementProcessing();

    // 2) 设置当前线程的记忆归属与对话上下文（冷读取摘要 sink 在 route 内部同步触发）
    const auto& targetMsg = batch.rawMessages.back();
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
            g_activeSenderId.clear();
            g_activeSenderName.clear();
            g_activePlatform.clear();
            g_activeGroupId.clear();
            g_activeKeepSilent = false;
            g_replyableMessageIds.clear();
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

    // 递增代际编号，宣告新消息到达（如果当前 unit 上有正在运行的旧 LLM 任务，将在此处触发其协作中断）
    const std::uint64_t myGen = unit->bumpGeneration();

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
            rawTurn.messageId = raw.messageId;
            rawTurn.replyToMessageId = raw.replyToMessageId;
            rawTurn.atUserIds = raw.atUserIds;
            achieve_.append(batch.conversation, rawTurn);
        }

        // 5) 批次内重组好的独立上下文 turns 追加进 unit 上下文
        for (auto& turn : batch.turns) {
            unit->append(std::move(turn));
        }

        // 6) 构建请求（当前回合已 append 进 unit；上下文不够 → 重新冷启动压缩）
        BuildInput in;
        in.systemPrompt = systemPrompt();
        in.unit = unit;
        in.now = now;
        auto result = currentBuilder->build(in);
        reColdStarted = result.reColdStarted;

        ChatRequest req = result.request;
        req.tools = registry_.defs();
        // Resolve identities on each request copy so cold-loaded histories and
        // contacts learned after the message arrived also have current names.
        // 只给"尚未回答"的输入附加身份提示：历史轮次保持逐字节稳定，提示词
        // 缓存前缀不会被联系人改名打断，也省掉重复的 token 开销。
        const auto hintFrom = history::pendingTurnStart(req.messages);
        for (std::size_t index = 0; index < req.messages.size(); ++index) {
            auto& turn = req.messages[index];
            if (turn.role == Role::User && !turn.messageId.empty() &&
                turn.platform == g_activePlatform &&
                ((batch.conversation.scope == ConversationScope::Group && turn.groupId == batch.conversation.id) ||
                 (batch.conversation.scope == ConversationScope::Private && turn.groupId.empty() &&
                  turn.senderId == batch.conversation.id)))
                g_replyableMessageIds.push_back(turn.messageId);
            if (turn.role != Role::User || turn.atUserIds.empty()) continue;
            if (index < hintFrom) continue;
            const auto hints = router_.mentionContext(turn.atUserIds, turn.platform);
            if (!hints.empty()) turn.text += "\n" + hints;
        }

        // 7) 工具循环；sink：逐条落档案 + 入 unit 上下文
        ToolLoopOptions loopOpt;
        loopOpt.shouldInterrupt = [unit, myGen]() {
            return unit->generation() != myGen;
        };

        std::optional<Msg> completion;
        resp = runToolLoop(*currentLlm, registry_, req, loopOpt,
                           [this, unit, &completion, conv = batch.conversation](
                                const Msg& m) {
                                // A final answer only completes the input after
                                // delivery succeeds; failed sends remain retryable.
                                if (m.role == Role::Assistant && m.toolCalls.empty()) {
                                    completion = m;
                                    return;
                                }
                                achieve_.append(conv, m);
                                unit->append(m);
                            });
        if (unit->generation() != myGen) {
            resp.finishReason = "interrupted";
            resp.text.clear();
        }
        if (resp.finishReason != "interrupted") {
            if (g_activeKeepSilent) {
                resp.text.clear();
            } else if (!resp.text.empty() && hasMessageSender()) {
                // 只有"确实有内容可发"时才把发送失败当成失败
                bool deliverable = false;
                {
                    std::lock_guard<std::mutex> lock(senderMtx_);
                    deliverable = hasDeliverableText(resp.text, splitterConfig_);
                }
                if (!deliverable) {
                    log::info("Runtime", "模型回复仅含空白或分段符，本轮按静默处理");
                    resp.text.clear();
                } else if (!emit(batch.conversation, targetMsg.senderId, resp.text)) {
                    if (unit->generation() != myGen) {
                        resp.finishReason = "interrupted";
                        resp.text.clear();
                    } else throw std::runtime_error("平台回复发送失败");
                }
            }
            if (resp.finishReason != "interrupted") {
                if (completion) {
                    achieve_.append(batch.conversation, *completion);
                    unit->append(*completion);
                }
                unit->stripEphemeralParts();
            }
        }
    }
    // 8) 锁外：状态快照 + 审计 + Facts 重建
    for (const auto& raw : batch.rawMessages) {
        noteMessage(raw.conversation, raw.senderId);
    }
    if (reColdStarted) {
        eventLog_.append({0, EventKind::SummaryApplied, now, "上下文重新冷启动"});
        // 冷启动同时消费日记：新认知顺带进 prompt（这一天已经变过一次前缀，
        // 边际成本为零）。注意 diary_.consume() 在取 services 锁之前求值。
        refreshFactsNow("reColdStart", diary_.consume());
    }

    if (resp.finishReason == "interrupted") {
        log::info("Runtime", "会话 " + batch.conversation.toString() +
                              " 的回复被新消息打断，丢弃过时输出");
        return BotReply{batch.conversation, "", /*interrupted=*/true};
    }

    if (g_activeKeepSilent) {
        log::info("Runtime", "模型自主调用 keepsilent 保持沉默，本轮不发送回复消息");
        resp.text = "";
    }

    return BotReply{batch.conversation, resp.text, /*interrupted=*/false};
}

RuntimeState Runtime::state() const {
    std::lock_guard<std::mutex> lock(stateMtx_);
    return state_;
}

} // namespace mio
