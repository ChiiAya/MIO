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
#include "core/time/LocalTime.h"
#include "log/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mio {
namespace {

constexpr int kMaxPeopleTokens = 1200;

static thread_local std::string g_activeChannelId;
static thread_local std::string g_activeActorId;
static thread_local std::string g_activeActorName;
static thread_local std::string g_activeSource;
static thread_local bool g_activeKeepSilent = false;
static thread_local std::vector<std::string> g_replyableMessageIds;
static thread_local bool g_maintenanceMode = false;

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

Runtime::Runtime(std::string botName, std::filesystem::path dataDir,
                 std::shared_ptr<Llm> llm,
                 std::shared_ptr<ConfigManager> configMgr)
    : dataDir_(std::move(dataDir)),
      persona_{botName, "", "", ""},
      graph_{dataDir_ / "relationships.json"},
      diary_{dataDir_ / "diary.json"},
      achieve_{dataDir_ / "history",
               (configMgr && configMgr->get()) ? configMgr->get()->achieve : AchieveConfig{}},
      gate_{&achieve_},
      configMgr_(configMgr ? std::move(configMgr) : std::make_shared<ConfigManager>()),
      llm_(llm ? std::move(llm) : std::make_shared<OpenAiCompat>(configMgr_->get()->openai)),
      embedding_(std::make_shared<OpenAiEmbedding>(configMgr_->get()->embedding)),
      summary_(std::make_shared<SummaryManager>(800, llm_)),
      builder_(std::make_shared<ContextBuilder>(configMgr_->get()->contextBuilder, summary_)) {
    log::initFromEnvironment();

    if (configMgr_ && configMgr_->get()) {
        auto cfg = configMgr_->get();
        if (!cfg->botName.empty() && botName == "Mio") persona_.botName = cfg->botName;
        if (!cfg->character.empty()) persona_.character = cfg->character;
        if (!cfg->systemPromptPrefix.empty()) persona_.systemPromptPrefix = cfg->systemPromptPrefix;
        if (!cfg->systemPromptNotice.empty()) persona_.systemPromptNotice = cfg->systemPromptNotice;
        splitterConfig_ = cfg->splitter;
        mindDebounceMs_ = std::max(0, cfg->mindDebounceMs);
        achieve_.setConfig(cfg->achieve);
    }

    registerBuiltinTools();

    mcpManager_ = std::make_unique<mcp::McpManager>();
    if (configMgr_ && configMgr_->get()) {
        mcpManager_->initialize(configMgr_->get()->mcp);
        mcpManager_->syncTools(registry_);
    }

    graph_.ensureMio(persona_.botName);

    summary_->setOnSummary(
        [this](const SummaryOutcome& outcome) { onSummaryToMemory(outcome); });

    // 冷读取历史事件进入单意识流上下文（受 AchieveConfig::enableColdSummary 控制）
    {
        std::lock_guard<std::mutex> lock(mindContextMtx_);
        mindContext_ = achieve_.coldRead("", summary_.get()).msgs;
    }

    refreshFactsNow("startup");
    state_.botName = persona_.botName;
    state_.messageCount = 0;
    state_.lastEventSeq = achieve_.maxId();

    int adminPort = (configMgr_ && configMgr_->get()) ? configMgr_->get()->adminPort : 6188;
    if (adminPort > 0) {
        adminServer_ = std::make_unique<AdminServer>(*this, adminPort);
        adminServer_->start();
    }

    if (configMgr_ && configMgr_->get()) {
        startSummaryBridge(configMgr_->get()->memoryBridge);
        startScheduler(configMgr_->get()->maintenance);
    }

    startMindLoop();
}

Runtime::Runtime(std::string botName, std::filesystem::path dataDir,
                 std::unique_ptr<Llm> llm)
    : Runtime(std::move(botName), std::move(dataDir),
              std::shared_ptr<Llm>(std::move(llm))) {}

Runtime::~Runtime() {
    if (scheduler_) {
        scheduler_->stop();
        scheduler_.reset();
    }
    stopMindLoop();
    if (summaryBridge_) {
        summaryBridge_->stop();
        summaryBridge_.reset();
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
        newSummary->setOnSummary(
            [this](const SummaryOutcome& outcome) { onSummaryToMemory(outcome); });
        auto newBuilder = std::make_shared<ContextBuilder>(newConfig->contextBuilder, newSummary);

        registry_.clearDynamicTools();
        if (mcpManager_) {
            mcpManager_->reload(newConfig->mcp, registry_);
        }

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
        achieve_.setConfig(newConfig->achieve);
        refreshFactsNow("config-reload");

        {
            std::lock_guard<std::mutex> lock(stateMtx_);
            state_.botName = persona_.botName;
        }
        {
            std::lock_guard<std::mutex> lock(senderMtx_);
            splitterConfig_ = newConfig->splitter;
        }
        {
            std::lock_guard<std::mutex> lock(mindLoopMtx_);
            mindDebounceMs_ = std::max(0, newConfig->mindDebounceMs);
        }

        startSummaryBridge(newConfig->memoryBridge);
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

void Runtime::setOutboundSender(OutboundSender sender) {
    std::lock_guard<std::mutex> lock(senderMtx_);
    outboundSender_ = std::move(sender);
}

void Runtime::setEventFilter(EventFilter filter) {
    std::lock_guard<std::mutex> lock(senderMtx_);
    eventFilter_ = std::move(filter);
}

void Runtime::setBeforeLlmHook(BeforeLlmHook hook) {
    std::lock_guard<std::mutex> lock(senderMtx_);
    beforeLlmHook_ = std::move(hook);
}

void Runtime::setOutboundFilter(OutboundFilter filter) {
    std::lock_guard<std::mutex> lock(senderMtx_);
    outboundFilter_ = std::move(filter);
}

void Runtime::setPluginManager(PluginManager* pm) {
    std::lock_guard<std::mutex> lock(senderMtx_);
    pluginManager_ = pm;
}

PluginManager* Runtime::pluginManager() const {
    std::lock_guard<std::mutex> lock(senderMtx_);
    return pluginManager_;
}

bool Runtime::hasMessageSender() const {
    std::lock_guard<std::mutex> lock(senderMtx_);
    return outboundSender_ != nullptr;
}

bool Runtime::emit(const std::string& channelId, const std::string& source, const std::string& rawText) {
    Outbound out;
    out.channelId = channelId;
    out.source = source;
    out.text = rawText;
    return emit(out);
}

bool Runtime::emit(const Outbound& rawOutbound) {
    Outbound outbound = rawOutbound;
    if (outbound.text.empty() && outbound.parts.empty()) return false;
    if (g_maintenanceMode) {
        log::warn("Runtime", "维护回合中禁止发送平台消息，已拒绝: " + outbound.channelId);
        return false;
    }

    OutboundSender sender;
    OutboundFilter outFilter;
    MessageSplitterConfig splitter;
    {
        std::lock_guard<std::mutex> lock(senderMtx_);
        sender = outboundSender_;
        outFilter = outboundFilter_;
        splitter = splitterConfig_;
    }
    if (outFilter && !outFilter(outbound)) {
        log::info("Runtime", "出站消息被 on_outbound Hook 拦截丢弃: " + outbound.channelId);
        return true;
    }
    const auto& rawText = outbound.text;
    if (rawText.empty() && outbound.parts.empty()) return false;
    if (!sender) {
        log::warn("Runtime", "未配置 OutboundSender，无法发送平台消息: " + rawText);
        return false;
    }

    std::vector<std::string> bubbles;
    if (splitter.enabled && !splitter.delimiter.empty() &&
        rawText.find(splitter.delimiter) != std::string::npos) {
        bubbles = splitByDelimiter(rawText, splitter.delimiter);
    } else {
        std::string s = rawText;
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
        if (!s.empty()) {
            bubbles.push_back(std::move(s));
        }
    }

    if (bubbles.empty() && outbound.parts.empty()) return false;
    if (bubbles.empty()) {
        bubbles.push_back("");
    }

    for (std::size_t i = 0; i < bubbles.size(); ++i) {
        if (i > 0 && splitter.enabled) {
            int delayMs = calculateDelayMs(bubbles[i - 1], splitter);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        Outbound bubble;
        bubble.source = outbound.source;
        bubble.channelId = outbound.channelId;
        bubble.text = bubbles[i];
        if (i == 0) {
            bubble.mentions = outbound.mentions;
            bubble.replyTo = outbound.replyTo;
            bubble.parts = outbound.parts;
        }
        if (!sender(bubble)) return false;
    }
    return true;
}

void Runtime::registerBuiltinTools() {
    registerContactTools(registry_, graph_, [this] {
        std::unique_lock<std::shared_mutex> lock(servicesMtx_);
        factsDirty_ = true;
        log::info("Runtime",
                  "通讯录已更新：按缓存前缀优先策略，systemPrompt 推迟到下一个刷新边界重建");
    });

    ToolDef timeDef;
    timeDef.name = "get_current_time";
    timeDef.description = "获取当前的本地日期与时间（精确到分钟）。";
    timeDef.parametersJsonSchema = {{"type", "object"},
                                    {"properties", nlohmann::json::object()}};
    registry_.add(std::move(timeDef),
                  [](const nlohmann::json&) -> std::string {
                      return "当前时间: " + nowTimeString();
                  }, ToolLayer::Builtin);

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
        return MessageToolContext{g_activeSource, g_activeChannelId};
    }, [this](const Outbound& outbound) {
        return emit(outbound);
    });

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

    auto waitHandler = [this](const nlohmann::json& args) -> std::string {
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
        const std::uint64_t waitInitialGen = mindGeneration_.load();
        while (elapsedMs < totalMs) {
            if (mindGeneration_.load() != waitInitialGen) {
                log::info("Runtime", "wait 过程中收到新事件打断，提前结束等待");
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

    // query_events —— 从 Achieve 全文事件库按时间窗口或事件 ID 检索完整详情
    ToolDef queryEvDef;
    queryEvDef.name = "query_events";
    queryEvDef.description =
        "从事件档案库（Achieve）按时间范围、最近时长（秒）或事件 ID 查询完整事件详情。"
        "当你在上下文中只看到 [Preview] 或 [Digest] 摘要而想查看具体消息正文时调用。";
    queryEvDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"target",
           {{"type", "string"},
            {"description", "可选过滤目标：source（如 napcat）、channelId（如 group:123 或 private:456）或 actorId；留空查全部"}}},
          {"recent_seconds",
           {{"type", "integer"},
            {"description", "查询最近 N 秒内的事件（如 300 表示最近 5 分钟）"}}},
          {"start_time",
           {{"type", "integer"},
            {"description", "起始 Unix 时间戳（秒或毫秒，配合 end_time 使用）"}}},
          {"end_time",
           {{"type", "integer"},
            {"description", "结束 Unix 时间戳（秒或毫秒，0 表示当前时间）"}}},
          {"event_id",
           {{"type", "integer"},
            {"description", "按单个事件序号 #id 精确查询"}}},
          {"limit",
           {{"type", "integer"},
            {"description", "最大返回条数（默认 30，上限 100）"}}}}}};
    registry_.add(
        std::move(queryEvDef),
        [this](const nlohmann::json& args) -> std::string {
            if (args.contains("event_id") && args["event_id"].is_number_integer()) {
                const auto id = args["event_id"].get<std::uint64_t>();
                if (auto ev = achieve_.getById(id)) {
                    return Achieve::formatEventsForTool({*ev});
                }
                return "未找到事件 #" + std::to_string(id);
            }
            const std::string target = args.value("target", "");
            std::size_t limit = static_cast<std::size_t>(std::clamp(args.value("limit", 30), 1, 100));
            if (args.contains("recent_seconds") && args["recent_seconds"].is_number_integer()) {
                const auto sec = args["recent_seconds"].get<std::int64_t>();
                return Achieve::formatEventsForTool(achieve_.queryRecentSeconds(target, sec, limit));
            }
            if (args.contains("start_time") && args["start_time"].is_number_integer()) {
                const auto start = args["start_time"].get<std::int64_t>();
                const auto end = args.value("end_time", std::int64_t{0});
                return Achieve::formatEventsForTool(achieve_.queryByTime(target, start, end, limit));
            }
            return Achieve::formatEventsForTool(achieve_.queryRecentSeconds(target, 600, limit));
        },
        ToolLayer::Builtin);

    // set_attention_rule —— 动态调整某群/某人/某来源的注意力层级
    ToolDef setRuleDef;
    setRuleDef.name = "set_attention_rule";
    setRuleDef.description =
        "设置注意力门规则，调整特定对象（pattern，如 group:123、private:456、actor:789、napcat）的注意力层级："
        "DirectFull（直接全文唤醒）、Preview（标题预览唤醒）、Digest（攒够一批再聚合唤醒）、Silent（静默归档不打扰）。";
    setRuleDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"pattern",
           {{"type", "string"},
            {"description", "匹配模式，如 group:123456、user:789、source:napcat"}}},
          {"tier",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"DirectFull", "Preview", "Digest", "Silent"})},
            {"description", "目标注意力层级"}}},
          {"duration_seconds",
           {{"type", "integer"},
            {"description", "规则生效时长（秒，0 或不填表示长期生效）"}}},
          {"reason",
           {{"type", "string"},
            {"description", "设置该规则的原因"}}}}},
        {"required", nlohmann::json::array({"pattern", "tier"})}};
    registry_.add(
        std::move(setRuleDef),
        [this](const nlohmann::json& args) -> std::string {
            const std::string pattern = args.value("pattern", "");
            const std::string tierStr = args.value("tier", "Preview");
            if (pattern.empty()) return "error: pattern 不能为空";
            auto tier = attentionTierFromString(tierStr);
            if (!tier) return "error: 无效的 tier: " + tierStr;
            AttentionRule rule;
            rule.pattern = pattern;
            rule.targetTier = *tier;
            rule.reason = args.value("reason", "");
            const std::int64_t dur = args.value("duration_seconds", std::int64_t{0});
            if (dur > 0) {
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                rule.expiresAt = nowMs + dur * 1000;
            }
            gate_.upsertRule(std::move(rule));
            return "已设置注意力规则: " + pattern + " -> " + attentionTierToString(*tier);
        },
        ToolLayer::Builtin);

    ToolDef listRuleDef;
    listRuleDef.name = "list_attention_rules";
    listRuleDef.description = "列出当前生效的所有注意力门规则。";
    listRuleDef.parametersJsonSchema = {{"type", "object"},
                                        {"properties", nlohmann::json::object()}};
    registry_.add(
        std::move(listRuleDef),
        [this](const nlohmann::json&) -> std::string {
            const auto rules = gate_.listRules();
            if (rules.empty()) return "当前无自定义注意力规则。";
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& r : rules) {
                arr.push_back({{"pattern", r.pattern},
                               {"tier", attentionTierToString(r.targetTier)},
                               {"expiresAt", r.expiresAt},
                               {"reason", r.reason}});
            }
            return arr.dump();
        },
        ToolLayer::Builtin);
}

std::string Runtime::rebuildSystemPrompt(
    const std::vector<DiaryEntry>& cognition) {
    if (!cognition.empty()) cognition_ = cognition;
    return buildSystemPrompt(persona_, graph_, cognition_, kMaxPeopleTokens);
}

void Runtime::refreshFactsNow(const std::string& reason,
                              std::vector<DiaryEntry> cognition) {
    std::unique_lock<std::shared_mutex> lock(servicesMtx_);
    const bool wasDirty = factsDirty_;
    systemPrompt_ = rebuildSystemPrompt(cognition);
    factsDirty_ = false;
    log::info("Runtime", "systemPrompt 重建: reason=" + reason +
                             " dirty=" + (wasDirty ? "true" : "false") +
                             " bytes=" + std::to_string(systemPrompt_.size()) +
                             " fp=" + promptFingerprint(systemPrompt_));
}

void Runtime::onSummaryToMemory(const SummaryOutcome& outcome) {
    if (!summaryBridge_ || outcome.text.empty()) return;
    SummaryBridge::Entry entry;
    entry.channelId = g_activeChannelId.empty() ? "mind" : g_activeChannelId;
    entry.text = outcome.text;
    entry.topic = outcome.topic;
    entry.isPublic = !outcome.privateVerdict;
    if (g_activeChannelId.rfind("group:", 0) == 0) {
        entry.groupId = g_activeChannelId.substr(6);
    }
    summaryBridge_->push(std::move(entry));
}

bool Runtime::writeSummaryToMemory(const SummaryBridge::Entry& entry) {
    const std::string& tool = summaryBridgeConfig_.tool;
    if (tool.empty() || !registry_.find(tool)) {
        if (!bridgeToolMissingLogged_) {
            bridgeToolMissingLogged_ = true;
            log::warn("Runtime", "记忆桥目标工具不在工具表中，摘要将不会写入记忆: " + tool +
                                     "（检查 mcp 配置与 memoryBridge.tool）");
        }
        return false;
    }
    nlohmann::json args = summaryBridgeConfig_.extraArgs.is_object()
                              ? summaryBridgeConfig_.extraArgs
                              : nlohmann::json::object();
    substitutePlaceholders(args, {{"text", entry.text},
                                  {"topic", entry.topic},
                                  {"conv", entry.channelId},
                                  {"group", entry.groupId},
                                  {"private", entry.isPublic ? "false" : "true"}});
    args[summaryBridgeConfig_.textField] = entry.text;
    registry_.invoke(tool, args);
    return true;
}

void Runtime::startSummaryBridge(const MemoryBridgeConfig& config) {
    if (summaryBridge_) {
        summaryBridge_->stop();
        summaryBridge_.reset();
    }
    bridgeToolMissingLogged_ = false;
    summaryBridgeConfig_ = config;
    if (!config.enabled) {
        log::info("Runtime", "摘要 → MCP 记忆桥未启用");
        return;
    }
    summaryBridge_ = std::make_unique<SummaryBridge>(
        config, [this](const SummaryBridge::Entry& entry) {
            return writeSummaryToMemory(entry);
        });
    summaryBridge_->start();
}

void Runtime::startScheduler(const MaintenanceConfig& config) {
    if (scheduler_) {
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

void Runtime::runMaintenancePrompt(const std::string& prompt) {
    if (prompt.empty()) return;

    std::shared_ptr<Llm> llm;
    std::shared_ptr<ContextBuilder> builder;
    std::string sysPrompt;
    {
        std::shared_lock<std::shared_mutex> lock(servicesMtx_);
        llm = llm_;
        builder = builder_;
        sysPrompt = systemPrompt_;
    }
    if (!llm || !builder) return;

    std::vector<Msg> workspace;
    {
        std::lock_guard<std::mutex> lock(mindContextMtx_);
        if (mindContext_.empty()) {
            log::info("Runtime", "维护回合跳过：当前意识流上下文为空");
            return;
        }
        workspace = mindContext_;
    }

    Msg ask{Role::User};
    ask.text = prompt;
    workspace.push_back(std::move(ask));

    ChatRequest req;
    try {
        BuildInput in;
        in.systemPrompt = sysPrompt;
        in.context = &workspace;
        req = builder->build(in).request;
    } catch (const std::exception& error) {
        log::error("Runtime", std::string("维护回合构建请求失败: ") + error.what());
        return;
    }
    req.tools = registry_.defs();

    struct MaintenanceGuard {
        MaintenanceGuard() { g_maintenanceMode = true; }
        ~MaintenanceGuard() { g_maintenanceMode = false; }
    } guard;

    ToolLoopOptions loopOpt;
    loopOpt.shouldInterrupt = [] { return false; };
    try {
        runToolLoop(*llm, registry_, req, loopOpt, [](const Msg&) {});
        log::info("Runtime", "维护回合完成");
    } catch (const std::exception& error) {
        log::error("Runtime", std::string("维护回合失败: ") + error.what());
    }
}

void Runtime::updateState(const Event& ev) {
    std::lock_guard<std::mutex> lock(stateMtx_);
    if (ev.id > 0) state_.lastEventSeq = ev.id;
}

void Runtime::noteEvent(const Event& ev) {
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        if (ev.category == EventCategory::ExternalMessage) {
            state_.messageCount++;
        }
        if (!ev.actorId.empty()) state_.activeUsers.insert(ev.actorId);
        if (!ev.channelId.empty()) state_.activeChannel = ev.channelId;
        state_.lastEventSeq = achieve_.maxId();
    }

    if (ev.category == EventCategory::ExternalMessage) {
        std::lock_guard<std::mutex> lock(metaMtx_);
        if (!ev.source.empty()) lastEventMeta_.source = ev.source;
        if (!ev.channelId.empty()) lastEventMeta_.channelId = ev.channelId;
        if (!ev.actorId.empty()) lastEventMeta_.actorId = ev.actorId;
        if (!ev.title.empty()) lastEventMeta_.title = ev.title;
        if (ev.data.is_object() && ev.data.contains("message_id") &&
            ev.data["message_id"].is_string()) {
            const std::string mid = ev.data["message_id"].get<std::string>();
            if (!mid.empty()) {
                lastEventMeta_.replyableIds.push_back(mid);
                if (lastEventMeta_.replyableIds.size() > 32) {
                    lastEventMeta_.replyableIds.erase(lastEventMeta_.replyableIds.begin());
                }
            }
        }
    }
}

void Runtime::postEvent(Event event) {
    EventFilter filter;
    {
        std::lock_guard<std::mutex> lock(senderMtx_);
        filter = eventFilter_;
    }
    if (filter && !filter(event)) {
        log::debug("Runtime", "入站事件被 on_event Hook 过滤丢弃");
        return;
    }

    const std::int64_t now = std::time(nullptr);
    if (event.category == EventCategory::ExternalMessage && !event.actorId.empty()) {
        const auto seen = graph_.onSeen(event.source, event.actorId, event.title, now);
        graph_.bumpIntimacy(graph_.mioId(), seen.internalId, 0.02, now);
        const std::string knownName = graph_.nameOf(seen.internalId);
        if (!knownName.empty() && knownName != seen.internalId) {
            event.title = knownName;
        }
    }

    const bool shouldWake = gate_.enterQueue(event);
    noteEvent(event);

    if (shouldWake) {
        mindGeneration_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(mindLoopMtx_);
            mindWakePending_ = true;
        }
        mindWakeCv_.notify_one();
    }
}

BotReply Runtime::flushMindLoop() {
    // 强制唤醒并等待 MindLoop 消费完当前所有待处理事件（跳过防抖等待）
    {
        std::lock_guard<std::mutex> lock(mindLoopMtx_);
        mindWakePending_ = true;
    }
    mindWakeCv_.notify_one();

    std::unique_lock<std::mutex> lock(mindLoopMtx_);
    mindIdleCv_.wait(lock, [this] {
        return (!mindWakePending_ && !mindProcessing_) || mindStop_;
    });
    return lastMindReply_;
}

void Runtime::startMindLoop() {
    std::lock_guard<std::mutex> lock(mindLoopMtx_);
    if (mindThread_.joinable()) return;
    mindStop_ = false;
    mindThread_ = std::thread(&Runtime::mindWorkerLoop, this);
}

void Runtime::stopMindLoop() {
    {
        std::lock_guard<std::mutex> lock(mindLoopMtx_);
        mindStop_ = true;
    }
    mindGeneration_.fetch_add(1);
    mindWakeCv_.notify_all();
    mindIdleCv_.notify_all();
    if (mindThread_.joinable()) {
        mindThread_.join();
    }
}

void Runtime::mindWorkerLoop() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mindLoopMtx_);
        mindWakeCv_.wait(lock, [this] { return mindWakePending_ || mindStop_; });
        if (mindStop_) break;

        // 防抖窗口：若在 mindDebounceMs_ 内有新唤醒事件到达，继续合并
        while (!mindStop_ && mindDebounceMs_ > 0) {
            const auto genBefore = mindGeneration_.load();
            const int debounce = mindDebounceMs_;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(debounce));
            lock.lock();
            if (mindGeneration_.load() == genBefore) break;
        }
        if (mindStop_) break;

        mindWakePending_ = false;
        mindProcessing_ = true;
        lock.unlock();

        try {
            BotReply reply = processMindTurn();
            std::lock_guard<std::mutex> doneLock(mindLoopMtx_);
            lastMindReply_ = std::move(reply);
        } catch (const std::exception& e) {
            log::error("Runtime", std::string("MindLoop 回合执行异常: ") + e.what());
        }

        lock.lock();
        mindProcessing_ = false;
        if (!mindWakePending_) {
            mindIdleCv_.notify_all();
        }
    }
}

BotReply Runtime::processMindTurn() {
    std::vector<Msg> incoming = gate_.drainMsgs(true);
    if (incoming.empty()) {
        return BotReply{};
    }

    std::shared_ptr<Llm> currentLlm;
    std::shared_ptr<ContextBuilder> currentBuilder;
    {
        std::shared_lock<std::shared_mutex> lock(servicesMtx_);
        currentLlm = llm_;
        currentBuilder = builder_;
    }
    if (!currentLlm || !currentBuilder) {
        throw std::runtime_error("Runtime 服务未就绪");
    }

    LastEventMeta meta;
    {
        std::lock_guard<std::mutex> lock(metaMtx_);
        meta = lastEventMeta_;
    }

    g_activeChannelId = meta.channelId;
    g_activeActorId = meta.actorId;
    g_activeActorName = meta.title;
    g_activeSource = meta.source;
    g_activeKeepSilent = false;
    g_replyableMessageIds = meta.replyableIds;

    struct ContextGuard {
        ~ContextGuard() {
            g_activeChannelId.clear();
            g_activeActorId.clear();
            g_activeActorName.clear();
            g_activeSource.clear();
            g_activeKeepSilent = false;
            g_replyableMessageIds.clear();
        }
    } contextGuard;

    const std::uint64_t myGen = mindGeneration_.load();
    ChatResponse resp;
    bool reColdStarted = false;

    {
        std::lock_guard<std::mutex> lock(mindContextMtx_);
        for (auto& m : incoming) {
            mindContext_.push_back(std::move(m));
        }

        BuildInput in;
        in.systemPrompt = systemPrompt();
        in.context = &mindContext_;
        auto result = currentBuilder->build(in);
        reColdStarted = result.reColdStarted;

        ChatRequest req = result.request;
        req.tools = registry_.defs();

        BeforeLlmHook beforeHook;
        {
            std::lock_guard<std::mutex> sLock(senderMtx_);
            beforeHook = beforeLlmHook_;
        }
        if (beforeHook) {
            beforeHook(req.messages);
        }

        ToolLoopOptions loopOpt;
        loopOpt.shouldInterrupt = [this, myGen]() {
            return mindGeneration_.load() != myGen;
        };

        std::optional<Msg> completion;
        resp = runToolLoop(*currentLlm, registry_, req, loopOpt,
                           [this, &completion](const Msg& m) {
                               if (m.role == Role::Assistant && m.toolCalls.empty()) {
                                   completion = m;
                                   return;
                               }
                               mindContext_.push_back(m);
                           });

        if (resp.text.empty() && mindGeneration_.load() != myGen) {
            resp.finishReason = "interrupted";
        }
        if (resp.finishReason != "interrupted" || !resp.text.empty()) {
            if (g_activeKeepSilent) {
                resp.text.clear();
            } else if (!resp.text.empty() && hasMessageSender() && !g_activeChannelId.empty()) {
                bool deliverable = false;
                {
                    std::lock_guard<std::mutex> sLock(senderMtx_);
                    deliverable = hasDeliverableText(resp.text, splitterConfig_);
                }
                if (!deliverable) {
                    log::info("Runtime", "模型回复仅含空白或分段符，本轮按静默处理");
                    resp.text.clear();
                } else if (!emit(g_activeChannelId, g_activeSource, resp.text)) {
                    throw std::runtime_error("平台回复发送失败");
                }
            }
            if (completion) {
                mindContext_.push_back(*completion);
            }
            history::stripCompletedMedia(mindContext_);
        }
    }

    if (reColdStarted) {
        refreshFactsNow("reColdStart", diary_.consume());
    }

    if (resp.finishReason == "interrupted" && resp.text.empty()) {
        log::info("Runtime", "MindLoop 当前回合被新事件打断，丢弃过时输出");
        return BotReply{meta.channelId, "", /*interrupted=*/true};
    }

    if (g_activeKeepSilent) {
        log::info("Runtime", "模型自主调用 keepsilent 保持沉默，本轮不发送回复消息");
        resp.text.clear();
    }

    return BotReply{meta.channelId, resp.text, /*interrupted=*/false};
}

RuntimeState Runtime::state() const {
    std::lock_guard<std::mutex> lock(stateMtx_);
    return state_;
}

} // namespace mio
