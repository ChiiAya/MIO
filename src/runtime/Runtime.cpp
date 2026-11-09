#include "runtime/Runtime.h"

#include "admin/AdminServer.h"
#include "log/Log.h"
#include "providers/llm/openai/OpenAiCompat.h"
#include "providers/mcp/McpManager.h"
#include "runtime/ContactTools.h"
#include "runtime/MessageTools.h"
#include "runtime/MindTools.h"

#include <ctime>
#include <utility>

namespace mio {

namespace
{
    constexpr const char* kTag = "Runtime";
    constexpr int kMaxPeopleTokens = 1200;
}

Runtime::Runtime(std::string botName,
                 std::filesystem::path dataDir,
                 std::shared_ptr<Llm> llm,
                 std::shared_ptr<ConfigManager> configMgr)
    : dataDir_(std::move(dataDir)),
      configMgr_(configMgr ? std::move(configMgr) : std::make_shared<ConfigManager>()),
      graph_(dataDir_ / "relationships.json"),
      achieve_(dataDir_ / "history",
               (configMgr_ && configMgr_->get()) ? configMgr_->get()->achieve : AchieveConfig{}),
      gate_(&achieve_),
      persona_{std::move(botName), "", "", ""} {
    log::initFromEnvironment();

    const auto cfg = configMgr_->get();

    // 先完成 LLM / ContextBuilder / Persona / systemPrompt 装配（此时 mindLoop_ 尚不存在，
    // refreshFactsNow 内部会跳过 updateServices）
    applyConfig(cfg ? *cfg : AppConfig{}, std::move(llm), "startup");

    // 冷读取历史事件作为初始意识流上下文，必须在 MindLoop 构造前移交给它
    std::vector<Msg> initialHistory = achieve_.coldRead("").msgs;

    mindLoop_ = std::make_unique<MindLoop>(MindLoop::MindLoopDeps{
        gate_,
        registry_,
        llm_,
        builder_,
        systemPrompt_,
        cfg ? cfg->splitter : MessageSplitterConfig{},
        std::move(initialHistory),
        cfg ? cfg->mindDebounceMs : 300,
        cfg ? cfg->autoEmitTextReply : false,
        cfg ? cfg->contextBuilder.rawKeep : 8,
        [this] {
            refreshFactsNow("reColdStart");
            return systemPrompt();
        },
        [this](const Outbound& outbound) { return emit(outbound); },
        [this](std::vector<Msg>& messages) {
            BeforeLlmHook hook;
            {
                std::lock_guard<std::mutex> lock(hooksMtx_);
                hook = beforeLlmHook_;
            }
            if (hook) hook(messages);
        }});

    registerBuiltinTools();

    mcpManager_ = std::make_unique<mcp::McpManager>();
    if (cfg) {
        mcpManager_->initialize(cfg->mcp);
        mcpManager_->syncTools(registry_);
    }

    graph_.ensureMio(persona_.botName);
    state_.lastEventSeq = achieve_.maxId();

    const int adminPort = cfg ? cfg->adminPort : 6188;
    if (adminPort > 0) {
        adminServer_ = std::make_unique<AdminServer>(*this, adminPort);
        adminServer_->start();
    }
}

Runtime::~Runtime() {
    if (mindLoop_) {
        mindLoop_->stop();
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

void Runtime::registerBuiltinTools() {
    registerContactTools(registry_, graph_, [this] {
        std::unique_lock<std::shared_mutex> lock(servicesMtx_);
        factsDirty_ = true;
        log::info(kTag, "通讯录已更新：按缓存前缀优先策略，systemPrompt 推迟到下一个刷新边界重建");
    });

    registerMessageTools(
        registry_,
        [this] { return mindLoop_->activeMessageContext(); },
        [this](const Outbound& outbound) -> bool {
            if (!emit(outbound)) return false;
            if (mindLoop_) mindLoop_->markMessageSentInTurn();
            return true;
        });

    registerMindTools(
        registry_,
        MindToolsDeps{
            achieve_,
            gate_,
            [this] { return mindLoop_ ? mindLoop_->generation() : 0; },
            [this] { if (mindLoop_) mindLoop_->markSilentInTurn(); }});
}

void Runtime::applyConfig(const AppConfig& cfg,
                          std::shared_ptr<Llm> customLlm,
                          const std::string& reason) {
    auto nextLlm = customLlm ? std::move(customLlm) : std::make_shared<OpenAiCompat>(cfg.openai);
    auto nextBuilder = std::make_shared<ContextBuilder>(cfg.contextBuilder);

    std::string botName;
    // 模型/端点决定上游缓存命名空间：切换后旧前缀缓存必然全部失效，需要压缩历史
    const std::string llmKey = cfg.openai.baseUrl + "\n" + cfg.openai.model;
    bool llmChanged = false;
    {
        std::unique_lock<std::shared_mutex> lock(servicesMtx_);
        llm_ = std::move(nextLlm);
        builder_ = std::move(nextBuilder);

        if (!cfg.botName.empty()) persona_.botName = cfg.botName;
        if (!cfg.character.empty()) persona_.character = cfg.character;
        if (!cfg.systemPromptPrefix.empty()) persona_.systemPromptPrefix = cfg.systemPromptPrefix;
        if (!cfg.systemPromptNotice.empty()) persona_.systemPromptNotice = cfg.systemPromptNotice;
        botName = persona_.botName;

        llmChanged = !activeLlmKey_.empty() && llmKey != activeLlmKey_;
        activeLlmKey_ = llmKey;
    }

    achieve_.setConfig(cfg.achieve);
    refreshFactsNow(reason);
    if (mindLoop_ && llmChanged) mindLoop_->requestCompact("模型/端点变更");

    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        state_.botName = botName;
    }
}

bool Runtime::reloadConfig(const std::filesystem::path& configPath) {
    if (!configMgr_) {
        log::warn(kTag, "ConfigManager 未初始化");
        return false;
    }
    if (!configMgr_->reloadFromFile(configPath)) {
        log::warn(kTag, "配置重载失败，保持当前配置运行: " + configPath.string());
        return false;
    }

    const auto nextCfg = configMgr_->get();
    try {
        if (mcpManager_) {
            mcpManager_->reload(nextCfg->mcp, registry_);
        }
        applyConfig(*nextCfg, /*customLlm=*/nullptr, "config-reload");
        log::info(kTag, "配置热重载成功: " + configPath.string());
        return true;
    } catch (const std::exception& e) {
        log::warn(kTag, std::string("组件重建异常，保持原配置: ") + e.what());
        return false;
    }
}

std::shared_ptr<const AppConfig> Runtime::config() const {
    return configMgr_ ? configMgr_->get() : nullptr;
}

std::string Runtime::systemPrompt() const {
    std::shared_lock<std::shared_mutex> lock(servicesMtx_);
    return systemPrompt_;
}

void Runtime::refreshFactsNow(const std::string& reason) {
    std::shared_ptr<Llm> llm;
    std::shared_ptr<ContextBuilder> builder;
    std::string prompt;
    {
        std::unique_lock<std::shared_mutex> lock(servicesMtx_);
        const bool wasDirty = factsDirty_;
        systemPrompt_ = buildSystemPrompt(persona_, graph_, kMaxPeopleTokens);
        factsDirty_ = false;
        log::info(kTag, "systemPrompt 重建: reason=" + reason +
                            " dirty=" + (wasDirty ? "true" : "false") +
                            " bytes=" + std::to_string(systemPrompt_.size()));
        llm = llm_;
        builder = builder_;
        prompt = systemPrompt_;
    }
    if (mindLoop_) {
        const auto cfg = config();
        mindLoop_->updateServices(std::move(llm), std::move(builder), std::move(prompt),
                                  cfg ? cfg->splitter : MessageSplitterConfig{},
                                  cfg ? cfg->mindDebounceMs : 300,
                                  cfg ? cfg->autoEmitTextReply : false,
                                  cfg ? cfg->contextBuilder.rawKeep : 8);
    }
}

void Runtime::postEvent(Event event) {
    EventFilter filter;
    {
        std::lock_guard<std::mutex> lock(hooksMtx_);
        filter = eventFilter_;
    }
    if (filter && !filter(event)) {
        log::debug(kTag, "入站事件被 on_event Hook 过滤丢弃");
        return;
    }

    const std::int64_t now = std::time(nullptr);
    if (event.category == EventCategory::ExternalMessage && !event.actorId.empty()) {
        const auto seen = graph_.onSeen(event.source, event.actorId, event.title, now);
        const std::string knownName = graph_.nameOf(seen.internalId);
        if (!knownName.empty() && knownName != seen.internalId) {
            event.title = knownName;
        }
    }

    const bool shouldWake = gate_.enterQueue(event);
    {
        std::lock_guard<std::mutex> lock(stateMtx_);
        if (event.category == EventCategory::ExternalMessage) {
            state_.messageCount++;
        }
        if (!event.actorId.empty()) state_.activeUsers.insert(event.actorId);
        if (!event.channelId.empty()) state_.activeChannel = event.channelId;
        state_.lastEventSeq = achieve_.maxId();
    }
    if (shouldWake && mindLoop_) {
        mindLoop_->notifyWake();
    }
}

RuntimeState Runtime::state() const {
    std::lock_guard<std::mutex> lock(stateMtx_);
    return state_;
}

void Runtime::setOutboundSender(OutboundSender sender) {
    std::lock_guard<std::mutex> lock(hooksMtx_);
    outboundSender_ = std::move(sender);
}

void Runtime::setEventFilter(EventFilter filter) {
    std::lock_guard<std::mutex> lock(hooksMtx_);
    eventFilter_ = std::move(filter);
}

void Runtime::setBeforeLlmHook(BeforeLlmHook hook) {
    std::lock_guard<std::mutex> lock(hooksMtx_);
    beforeLlmHook_ = std::move(hook);
}

void Runtime::setPluginManager(PluginManager* pm) {
    std::lock_guard<std::mutex> lock(hooksMtx_);
    pluginManager_ = pm;
}

PluginManager* Runtime::pluginManager() const {
    std::lock_guard<std::mutex> lock(hooksMtx_);
    return pluginManager_;
}

bool Runtime::hasMessageSender() const {
    std::lock_guard<std::mutex> lock(hooksMtx_);
    return outboundSender_ != nullptr;
}

bool Runtime::emit(const std::string& channelId, const std::string& source, const std::string& rawText) {
    Outbound out;
    out.channelId = channelId;
    out.source = source;
    out.text = rawText;
    return emit(out);
}

bool Runtime::emit(const Outbound& outbound) {
    if (outbound.text.empty() && outbound.parts.empty()) return false;
    if (outbound.channelId.empty()) {
        log::warn(kTag, "出站消息缺少目标 channelId，已拒绝");
        return false;
    }
    OutboundSender sender;
    {
        std::lock_guard<std::mutex> lock(hooksMtx_);
        sender = outboundSender_;
    }
    if (!sender) {
        log::warn(kTag, "未配置 OutboundSender，无法发送平台消息: " + outbound.text);
        return false;
    }
    if (!sender(outbound)) return false;

    // 发送成功后归档自身回复（仅 text），供重启后冷读恢复为 assistant 历史
    Event ev;
    ev.category = EventCategory::InternalPulse;
    ev.tier = AttentionTier::DirectFull;
    ev.source = outbound.source;
    ev.channelId = outbound.channelId;
    ev.actorId = "self";
    ev.content = outbound.text;
    ev.timestamp = static_cast<std::int64_t>(std::time(nullptr)) * 1000;
    ev.data = {{"role", "assistant"}};
    achieve_.append(std::move(ev));
    return true;
}

} // namespace mio
