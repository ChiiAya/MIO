#include "plugin/PluginManager.h"
#include "plugin/HostApi.h"

#include "config/AppConfig.h"
#include "core/event/Event.h"
#include "log/Log.h"
#include "mio/plugin.hpp"
#include "runtime/Runtime.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace mio {
namespace {

std::string strFromMio(mio_str s) {
    if (!s.data || s.size == 0) return {};
    return std::string(s.data, s.size);
}

mio_str strToMio(const std::string& s) {
    return mio_str{s.data(), s.size()};
}

void hostLog(void* rawCtx, mio_log_level lv, mio_str tag, mio_str msg) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    const std::string tagStr = (tag.data && tag.size > 0)
                                   ? strFromMio(tag)
                                   : (ctx ? ctx->pluginId : "Plugin");
    const std::string msgStr = strFromMio(msg);
    switch (lv) {
    case MIO_LOG_DEBUG: log::debug(tagStr, msgStr); break;
    case MIO_LOG_INFO:  log::info(tagStr, msgStr);  break;
    case MIO_LOG_WARN:  log::warn(tagStr, msgStr);  break;
    case MIO_LOG_ERROR: log::error(tagStr, msgStr); break;
    }
}

int hostPostEvent(void* rawCtx, mio_str event_json) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->runtime || !event_json.data || event_json.size == 0) return -1;
    try {
        const auto j = nlohmann::json::parse(event_json.data, event_json.data + event_json.size);
        Event ev = Event::fromJson(j);
        if (ev.source.empty()) {
            ev.source = ctx->pluginId;
        }
        ctx->runtime->postEvent(std::move(ev));
        return 0;
    } catch (const std::exception& e) {
        log::warn(ctx->pluginId, std::string("post_event 解析失败: ") + e.what());
        return -1;
    }
}

int hostRegisterTool(void* rawCtx, mio_str def_json, mio_tool_fn fn, void* user) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->runtime || !fn || !def_json.data || def_json.size == 0) return -1;
    try {
        const auto j = nlohmann::json::parse(def_json.data, def_json.data + def_json.size);
        ToolDef def;
        def.name = j.value("name", "");
        def.description = j.value("description", "");
        if (j.contains("parameters") && j["parameters"].is_object()) {
            def.parametersJsonSchema = j["parameters"];
        } else {
            def.parametersJsonSchema = {{"type", "object"},
                                        {"properties", nlohmann::json::object()}};
        }
        def.isTerminal = j.value("terminal", false);
        if (def.name.empty()) return -1;

        std::string owner = j.value("owner", ctx->owner);
        ctx->runtime->toolRegistry().add(
            std::move(def),
            [fn, user](const nlohmann::json& args) -> std::string {
                const std::string dumped = args.dump();
                std::string out;
                fn(user, strToMio(dumped), &out, &sdk::detail::stringSinkWrite);
                return out;
            },
            ToolLayer::Dynamic,
            std::move(owner));
        return 0;
    } catch (const std::exception& e) {
        log::warn(ctx->pluginId, std::string("register_tool 失败: ") + e.what());
        return -1;
    }
}

int hostUnregisterTool(void* rawCtx, mio_str name) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->runtime) return -1;
    const std::string toolName = strFromMio(name);
    if (toolName.empty()) return -1;
    return ctx->runtime->toolRegistry().remove(toolName) ? 0 : -1;
}

int hostRegisterChannel(void* rawCtx, mio_str source, const mio_channel_vtbl* vt, void* user) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->manager || !vt || !vt->send) return -1;
    std::string src = strFromMio(source);
    if (src.empty()) src = ctx->pluginId;
    return ctx->manager->registerChannel(ctx->owner, src, vt, user) ? 0 : -1;
}

int hostInvokeTool(void* rawCtx, mio_str name, mio_str args_json, void* sink, mio_write_fn write) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->runtime) return -1;
    try {
        const std::string toolName = strFromMio(name);
        nlohmann::json args = nlohmann::json::object();
        if (args_json.data && args_json.size > 0) {
            args = nlohmann::json::parse(args_json.data, args_json.data + args_json.size);
        }
        const std::string result = ctx->runtime->toolRegistry().invoke(toolName, args);
        if (write && !result.empty()) {
            write(sink, strToMio(result));
        }
        return 0;
    } catch (const std::exception& e) {
        const std::string err = std::string("error: ") + e.what();
        if (write) write(sink, strToMio(err));
        return -1;
    }
}

void hostDataDir(void* rawCtx, void* sink, mio_write_fn write) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !write) return;
    std::error_code ec;
    std::filesystem::create_directories(ctx->dataDir, ec);
    const std::string pathStr = ctx->dataDir.string();
    write(sink, strToMio(pathStr));
}

int hostReloadConfig(void* rawCtx, mio_str config_path) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->runtime) return -1;
    std::string path = strFromMio(config_path);
    if (path.empty()) path = "config.json";
    return ctx->runtime->reloadConfig(path) ? 0 : -1;
}

void hostStateJson(void* rawCtx, void* sink, mio_write_fn write) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->runtime || !write) return;
    const auto st = ctx->runtime->state();
    nlohmann::json j = {
        {"version", st.version},
        {"messageCount", st.messageCount},
        {"botName", st.botName},
        {"lastEventSeq", st.lastEventSeq},
        {"activeChannel", st.activeChannel.value_or("none")},
        {"activeUsersCount", st.activeUsers.size()}
    };
    const std::string dumped = j.dump();
    write(sink, strToMio(dumped));
}

void hostRequestShutdown(void* rawCtx) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->manager) return;
    ctx->manager->requestShutdown();
}

void hostFlushMindLoop(void* /*rawCtx*/, void* sink, mio_write_fn write) {
    if (write) {
        const std::string dumped = "{}";
        write(sink, strToMio(dumped));
    }
}

int hostRegisterHook(void* rawCtx, mio_str hook_point, int priority, mio_hook_fn fn, void* user) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->manager || !fn) return -1;
    const std::string point = strFromMio(hook_point);
    if (point.empty()) return -1;
    return ctx->manager->registerHook(ctx->owner, point, priority, fn, user) ? 0 : -1;
}

void hostPluginsJson(void* rawCtx, void* sink, mio_write_fn write) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->manager || !write) return;
    const std::string dumped = ctx->manager->listPluginsJson().dump();
    write(sink, strToMio(dumped));
}

int hostReloadPlugin(void* rawCtx, mio_str plugin_id) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->manager) return -1;
    const std::string id = strFromMio(plugin_id);
    return ctx->manager->reloadPlugin(id) ? 0 : -1;
}

} // namespace

void initHostApi(PluginHostContext& ctx) {
    ctx.api.struct_size = static_cast<uint32_t>(sizeof(mio_host_api));
    ctx.api.abi_version = MIO_PLUGIN_ABI_VERSION;
    ctx.api.ctx = &ctx;
    ctx.api.log = &hostLog;
    ctx.api.post_event = &hostPostEvent;
    ctx.api.register_tool = &hostRegisterTool;
    ctx.api.unregister_tool = &hostUnregisterTool;
    ctx.api.register_channel = &hostRegisterChannel;
    ctx.api.invoke_tool = &hostInvokeTool;
    ctx.api.data_dir = &hostDataDir;
    ctx.api.reload_config = &hostReloadConfig;
    ctx.api.state_json = &hostStateJson;
    ctx.api.request_shutdown = &hostRequestShutdown;
    ctx.api.flush_mind_loop = &hostFlushMindLoop;
    ctx.api.register_hook = &hostRegisterHook;
    ctx.api.plugins_json = &hostPluginsJson;
    ctx.api.reload_plugin = &hostReloadPlugin;
}

PluginManager::PluginManager(Runtime& runtime) : runtime_(runtime) {
    runtime_.setPluginManager(this);
    runtime_.setOutboundSender([this](const Outbound& outbound) {
        return sendOutbound(outbound);
    });
    runtime_.setEventFilter([this](Event& ev) {
        return runEventHooks(ev);
    });
    runtime_.setBeforeLlmHook([this](std::vector<Msg>& messages) {
        runBeforeLlmHooks(messages);
    });
}

PluginManager::~PluginManager() {
    runtime_.setPluginManager(nullptr);
    stopAll();
}

void PluginManager::loadAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    auto cfg = runtime_.config();
    std::string envPlatform;
    if (const char* p = std::getenv("MIO_PLATFORM")) {
        if (*p != '\0') envPlatform = p;
    }

    const auto& staticDescs = sdk::staticPluginRegistry();
    for (const mio_plugin_desc* desc : staticDescs) {
        if (!desc || !desc->id || !desc->create) continue;
        if (desc->abi_version != MIO_PLUGIN_ABI_VERSION) {
            log::warn("PluginManager", std::string("插件 ABI 版本不匹配: ") + desc->id);
            continue;
        }

        const std::string id = desc->id;
        bool enabled = false;
        nlohmann::json pluginCfg = nlohmann::json::object();

        if (!envPlatform.empty()) {
            enabled = (envPlatform == "all" || envPlatform == "both" ||
                       envPlatform == id || id == "lua-host");
            if (cfg) {
                for (const auto& spec : cfg->plugins) {
                    if (spec.id == id) {
                        pluginCfg = spec.config;
                        break;
                    }
                }
            }
        } else if (cfg && !cfg->plugins.empty()) {
            for (const auto& spec : cfg->plugins) {
                if (spec.id == id) {
                    enabled = spec.enabled;
                    pluginCfg = spec.config;
                    break;
                }
            }
        } else {
            const std::string plat = cfg ? cfg->platform : "console";
            enabled = (plat == "all" || plat == "both" || plat == id || id == "lua-host");
        }

        if (!enabled) {
            continue;
        }

        auto hostCtx = std::make_unique<PluginHostContext>();
        hostCtx->pluginId = id;
        hostCtx->owner = "plugin:" + id;
        hostCtx->dataDir = runtime_.dataDir() / "plugins" / id;
        hostCtx->runtime = &runtime_;
        hostCtx->manager = this;
        initHostApi(*hostCtx);

        const std::string cfgStr = pluginCfg.dump();
        void* instance = desc->create(&hostCtx->api, strToMio(cfgStr));
        if (!instance) {
            log::error("PluginManager", "创建插件实例失败: " + id);
            continue;
        }

        LoadedPlugin loaded;
        loaded.desc = desc;
        loaded.hostCtx = std::move(hostCtx);
        loaded.instance = instance;
        loaded.started = false;
        loaded.config = std::move(pluginCfg);
        plugins_.push_back(std::move(loaded));
        log::info("PluginManager", "已加载插件: " + id + " v" + (desc->version ? desc->version : "0.0.0"));
    }
}

void PluginManager::startAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& p : plugins_) {
        if (p.started || !p.desc || !p.desc->start) continue;
        if (p.desc->start(p.instance) == 0) {
            p.started = true;
            log::info("PluginManager", std::string("已启动插件: ") + p.desc->id);
        } else {
            log::error("PluginManager", std::string("启动插件失败: ") + p.desc->id);
        }
    }
}

void PluginManager::stopAll() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (it->started && it->desc && it->desc->stop) {
            it->desc->stop(it->instance);
            it->started = false;
            log::info("PluginManager", std::string("已停止插件: ") + it->desc->id);
        }
        if (it->hostCtx) {
            unregisterChannelsByOwner(it->hostCtx->owner);
            unregisterHooksByOwner(it->hostCtx->owner);
            runtime_.toolRegistry().removeByOwner(it->hostCtx->owner);
        }
        if (it->instance && it->desc && it->desc->destroy) {
            it->desc->destroy(it->instance);
            it->instance = nullptr;
        }
    }
    plugins_.clear();
}

nlohmann::json PluginManager::listPluginsJson() const {
    std::lock_guard<std::mutex> lock(mtx_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : plugins_) {
        if (!p.desc || !p.desc->id) continue;
        const std::string id = p.desc->id;
        const std::string owner = p.hostCtx ? p.hostCtx->owner : ("plugin:" + id);
        arr.push_back({
            {"id", id},
            {"version", p.desc->version ? p.desc->version : "0.0.0"},
            {"started", p.started},
            {"owner", owner},
            {"tools", runtime_.toolRegistry().namesByOwner(owner)}
        });
    }
    return arr;
}

bool PluginManager::reloadPlugin(const std::string& pluginId) {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::string target = pluginId.empty() ? "lua-host" : pluginId;
    bool anyReloaded = false;

    for (auto& p : plugins_) {
        if (!p.desc || !p.desc->id) continue;
        const std::string id = p.desc->id;
        if (target != "all" && target != id &&
            !(id == "lua-host" && target.rfind("lua:", 0) == 0)) {
            continue;
        }

        nlohmann::json cfgJson = p.config;
        if (target.rfind("lua:", 0) == 0) {
            cfgJson["reload_script"] = target.substr(4);
        }
        const std::string dumped = cfgJson.dump();

        if (p.desc->reconfigure && p.desc->reconfigure(p.instance, strToMio(dumped)) == 0) {
            log::info("PluginManager", "插件已热重载 (reconfigure): " + id);
            anyReloaded = true;
            continue;
        }

        // 若不支持 reconfigure，则执行 stop -> destroy -> create -> start
        if (p.started && p.desc->stop) {
            p.desc->stop(p.instance);
            p.started = false;
        }
        if (p.hostCtx) {
            unregisterChannelsByOwner(p.hostCtx->owner);
            unregisterHooksByOwner(p.hostCtx->owner);
            runtime_.toolRegistry().removeByOwner(p.hostCtx->owner);
        }
        if (p.instance && p.desc->destroy) {
            p.desc->destroy(p.instance);
            p.instance = nullptr;
        }

        p.instance = p.desc->create(&p.hostCtx->api, strToMio(dumped));
        if (p.instance && p.desc->start && p.desc->start(p.instance) == 0) {
            p.started = true;
            log::info("PluginManager", "插件已重启热重载: " + id);
            anyReloaded = true;
        } else {
            log::error("PluginManager", "插件热重载失败: " + id);
        }
    }
    return anyReloaded;
}

void PluginManager::waitForShutdown() {
    std::unique_lock<std::mutex> lock(shutdownMtx_);
    shutdownCv_.wait(lock, [this] { return shutdownRequested_.load(); });
}

void PluginManager::requestShutdown() {
    shutdownRequested_.store(true);
    shutdownCv_.notify_all();
}

bool PluginManager::registerChannel(const std::string& owner,
                                    const std::string& source,
                                    const mio_channel_vtbl* vt,
                                    void* user) {
    return outboundRouter_.registerChannel(owner, source, vt, user);
}

void PluginManager::unregisterChannelsByOwner(const std::string& owner) {
    outboundRouter_.unregisterByOwner(owner);
}

bool PluginManager::sendOutbound(const Outbound& rawOutbound) {
    Outbound outbound = rawOutbound;
    if (outbound.text.empty() && outbound.parts.empty()) return false;
    if (!runOutboundHooks(outbound)) {
        log::info("PluginManager", "出站消息被 on_outbound Hook 拦截丢弃: " + outbound.channelId);
        return true;
    }
    if (outbound.text.empty() && outbound.parts.empty()) return false;
    auto cfg = runtime_.config();
    const MessageSplitterConfig splitter = cfg ? cfg->splitter : MessageSplitterConfig{};
    return outboundRouter_.emit(outbound, splitter);
}

bool PluginManager::registerHook(const std::string& owner,
                                 const std::string& hookPoint,
                                 int priority,
                                 mio_hook_fn fn,
                                 void* user) {
    if (hookPoint.empty() || !fn) return false;
    std::lock_guard<std::mutex> lock(hookMtx_);
    auto& list = hooks_[hookPoint];
    list.push_back(HookEntry{owner, hookPoint, priority, fn, user});
    std::stable_sort(list.begin(), list.end(),
                     [](const HookEntry& a, const HookEntry& b) {
                         return a.priority < b.priority;
                     });
    log::info("PluginManager", "已注册 Hook: point=" + hookPoint +
                                   " priority=" + std::to_string(priority) +
                                   " (" + owner + ")");
    return true;
}

void PluginManager::unregisterHooksByOwner(const std::string& owner) {
    std::lock_guard<std::mutex> lock(hookMtx_);
    for (auto& [_, list] : hooks_) {
        list.erase(
            std::remove_if(list.begin(), list.end(),
                           [&](const HookEntry& e) { return e.owner == owner; }),
            list.end());
    }
}

bool PluginManager::runEventHooks(Event& ev) {
    std::vector<HookEntry> chain;
    {
        std::lock_guard<std::mutex> lock(hookMtx_);
        auto it = hooks_.find("on_event");
        if (it == hooks_.end() || it->second.empty()) return true;
        chain = it->second;
    }

    nlohmann::json payload = ev.toJson();
    for (const auto& hook : chain) {
        if (!hook.fn) continue;
        const std::string inStr = payload.dump();
        std::string outStr;
        const int rc = hook.fn(hook.user, strToMio(inStr), &outStr, &sdk::detail::stringSinkWrite);
        if (rc > 0) {
            return false; // 被拦截丢弃
        }
        if (rc == 0 && !outStr.empty()) {
            try {
                payload = nlohmann::json::parse(outStr);
            } catch (const std::exception& e) {
                log::warn("PluginManager", "on_event Hook 返回非法 JSON (" + hook.owner + "): " + e.what());
            }
        }
    }
    ev = Event::fromJson(payload);
    return true;
}

void PluginManager::runBeforeLlmHooks(std::vector<Msg>& messages) {
    std::vector<HookEntry> chain;
    {
        std::lock_guard<std::mutex> lock(hookMtx_);
        auto it = hooks_.find("on_before_llm");
        if (it == hooks_.end() || it->second.empty()) return;
        chain = it->second;
    }

    nlohmann::json payload = {{"messageCount", messages.size()}};
    for (const auto& hook : chain) {
        if (!hook.fn) continue;
        const std::string inStr = payload.dump();
        std::string outStr;
        const int rc = hook.fn(hook.user, strToMio(inStr), &outStr, &sdk::detail::stringSinkWrite);
        if (rc == 0 && !outStr.empty()) {
            try {
                auto j = nlohmann::json::parse(outStr);
                if (j.is_object() && j.contains("extraSystemHint") && j["extraSystemHint"].is_string()) {
                    const std::string hint = j["extraSystemHint"].get<std::string>();
                    if (!hint.empty()) {
                        Msg hintMsg{Role::System};
                        hintMsg.text = hint;
                        messages.push_back(std::move(hintMsg));
                    }
                }
            } catch (...) {
            }
        }
    }
}

bool PluginManager::runOutboundHooks(Outbound& outbound) {
    std::vector<HookEntry> chain;
    {
        std::lock_guard<std::mutex> lock(hookMtx_);
        auto it = hooks_.find("on_outbound");
        if (it == hooks_.end() || it->second.empty()) return true;
        chain = it->second;
    }

    nlohmann::json payload = {
        {"source", outbound.source},
        {"channelId", outbound.channelId},
        {"text", outbound.text},
        {"replyTo", outbound.replyTo},
        {"mentions", outbound.mentions}
    };

    for (const auto& hook : chain) {
        if (!hook.fn) continue;
        const std::string inStr = payload.dump();
        std::string outStr;
        const int rc = hook.fn(hook.user, strToMio(inStr), &outStr, &sdk::detail::stringSinkWrite);
        if (rc > 0) {
            return false; // 拦截出站消息
        }
        if (rc == 0 && !outStr.empty()) {
            try {
                payload = nlohmann::json::parse(outStr);
            } catch (const std::exception& e) {
                log::warn("PluginManager", "on_outbound Hook 返回非法 JSON (" + hook.owner + "): " + e.what());
            }
        }
    }

    outbound.source = payload.value("source", outbound.source);
    outbound.channelId = payload.value("channelId", outbound.channelId);
    outbound.text = payload.value("text", outbound.text);
    outbound.replyTo = payload.value("replyTo", outbound.replyTo);
    return true;
}

} // namespace mio
