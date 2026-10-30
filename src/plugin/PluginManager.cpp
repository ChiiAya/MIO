#include "plugin/PluginManager.h"
#include "plugin/HostApi.h"

#include "config/AppConfig.h"
#include "core/event/Event.h"
#include "log/Log.h"
#include "mio/plugin.hpp"
#include "runtime/Runtime.h"

#include <nlohmann/json.hpp>

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

        ctx->runtime->toolRegistry().add(
            std::move(def),
            [fn, user](const nlohmann::json& args) -> std::string {
                const std::string dumped = args.dump();
                std::string out;
                fn(user, strToMio(dumped), &out, &sdk::detail::stringSinkWrite);
                return out;
            },
            ToolLayer::Dynamic,
            ctx->owner);
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
    return ctx->runtime->toolRegistry().remove(toolName, ctx->owner) ? 0 : -1;
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

void hostFlushMindLoop(void* rawCtx, void* sink, mio_write_fn write) {
    auto* ctx = static_cast<PluginHostContext*>(rawCtx);
    if (!ctx || !ctx->runtime) return;
    const BotReply reply = ctx->runtime->flushMindLoop();
    if (write) {
        nlohmann::json j = {
            {"channelId", reply.channelId},
            {"text", reply.text},
            {"interrupted", reply.interrupted}
        };
        const std::string dumped = j.dump();
        write(sink, strToMio(dumped));
    }
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
}

PluginManager::PluginManager(Runtime& runtime) : runtime_(runtime) {
    runtime_.setOutboundSender([this](const Outbound& outbound) {
        return sendOutbound(outbound);
    });
}

PluginManager::~PluginManager() {
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
            runtime_.toolRegistry().removeByOwner(it->hostCtx->owner);
        }
        if (it->instance && it->desc && it->desc->destroy) {
            it->desc->destroy(it->instance);
            it->instance = nullptr;
        }
    }
    plugins_.clear();
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

bool PluginManager::sendOutbound(const Outbound& outbound) {
    return outboundRouter_.sendBubble(outbound);
}

} // namespace mio
