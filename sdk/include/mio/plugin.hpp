#pragma once
// ============================================================================
// MIO C++ Plugin SDK (header-only wrapper over C ABI `mio/plugin_abi.h`)
// ============================================================================

#include "mio/plugin_abi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mio::sdk {

namespace detail {

inline mio_str toMioStr(std::string_view sv) noexcept {
    return mio_str{sv.data(), sv.size()};
}

inline std::string fromMioStr(mio_str s) {
    if (!s.data || s.size == 0) return {};
    return std::string(s.data, s.size);
}

inline void stringSinkWrite(void* sink, mio_str bytes) {
    if (!sink || !bytes.data || bytes.size == 0) return;
    static_cast<std::string*>(sink)->append(bytes.data, bytes.size);
}

} // namespace detail

class Channel {
public:
    virtual ~Channel() = default;
    virtual bool send(const nlohmann::json& outbound) = 0;
    virtual nlohmann::json capabilities() const { return nlohmann::json::object(); }
};

class Host {
public:
    explicit Host(const mio_host_api* raw) : raw_(raw) {}

    void log(mio_log_level lv, std::string_view tag, std::string_view msg) const {
        if (raw_ && raw_->log) {
            raw_->log(raw_->ctx, lv, detail::toMioStr(tag), detail::toMioStr(msg));
        }
    }

    void debug(std::string_view tag, std::string_view msg) const { log(MIO_LOG_DEBUG, tag, msg); }
    void info(std::string_view tag, std::string_view msg) const  { log(MIO_LOG_INFO, tag, msg); }
    void warn(std::string_view tag, std::string_view msg) const  { log(MIO_LOG_WARN, tag, msg); }
    void error(std::string_view tag, std::string_view msg) const { log(MIO_LOG_ERROR, tag, msg); }

    bool postEvent(const nlohmann::json& ev) const {
        if (!raw_ || !raw_->post_event) return false;
        const std::string dumped = ev.dump();
        return raw_->post_event(raw_->ctx, detail::toMioStr(dumped)) == 0;
    }

    using ToolHandler = std::function<std::string(const nlohmann::json& args)>;

    bool registerTool(const nlohmann::json& def, ToolHandler fn) {
        if (!raw_ || !raw_->register_tool || !fn) return false;
        auto holder = std::make_unique<ToolHolder>(ToolHolder{def.value("name", ""), std::move(fn)});
        ToolHolder* ptr = holder.get();
        const std::string dumped = def.dump();
        const int rc = raw_->register_tool(raw_->ctx, detail::toMioStr(dumped), &Host::toolThunk, ptr);
        if (rc == 0) {
            tools_.push_back(std::move(holder));
            return true;
        }
        return false;
    }

    bool unregisterTool(std::string_view name) {
        if (!raw_ || !raw_->unregister_tool) return false;
        if (raw_->unregister_tool(raw_->ctx, detail::toMioStr(name)) != 0) return false;
        tools_.erase(std::remove_if(tools_.begin(), tools_.end(),
                                    [name](const std::unique_ptr<ToolHolder>& holder) {
                                        return holder->name == name;
                                    }),
                     tools_.end());
        return true;
    }

    bool registerChannel(std::string_view source, Channel* ch) {
        if (!raw_ || !raw_->register_channel || !ch) return false;
        static const mio_channel_vtbl kVtbl = {
            static_cast<uint32_t>(sizeof(mio_channel_vtbl)),
            &Host::channelSendThunk,
            &Host::channelCapThunk
        };
        return raw_->register_channel(raw_->ctx, detail::toMioStr(source), &kVtbl, ch) == 0;
    }

    std::string invokeTool(std::string_view name, const nlohmann::json& args) const {
        if (!raw_ || !raw_->invoke_tool) return {};
        std::string out;
        const std::string dumped = args.dump();
        raw_->invoke_tool(raw_->ctx, detail::toMioStr(name), detail::toMioStr(dumped),
                          &out, &detail::stringSinkWrite);
        return out;
    }

    std::filesystem::path dataDir() const {
        if (!raw_ || !raw_->data_dir) return {};
        std::string out;
        raw_->data_dir(raw_->ctx, &out, &detail::stringSinkWrite);
        return std::filesystem::path(out);
    }

    bool reloadConfig(std::string_view path = "config.json") const {
        if (!raw_ || !raw_->reload_config) return false;
        return raw_->reload_config(raw_->ctx, detail::toMioStr(path)) == 0;
    }

    nlohmann::json stateJson() const {
        if (!raw_ || !raw_->state_json) return nlohmann::json::object();
        std::string out;
        raw_->state_json(raw_->ctx, &out, &detail::stringSinkWrite);
        try {
            if (!out.empty()) return nlohmann::json::parse(out);
        } catch (...) {
        }
        return nlohmann::json::object();
    }

    void requestShutdown() const {
        if (raw_ && raw_->request_shutdown) {
            raw_->request_shutdown(raw_->ctx);
        }
    }

    void refreshFacts() const {
        if (raw_ && raw_->refresh_facts) raw_->refresh_facts(raw_->ctx);
    }

    // HookHandler:
    //   drop 置为 true 表示拦截丢弃；
    //   返回 std::nullopt 表示放行原载荷；返回非空 json 表示修改载荷后放行。
    using HookHandler = std::function<std::optional<nlohmann::json>(const nlohmann::json& payload, bool& drop)>;

    bool registerHook(std::string_view hookPoint, int priority, HookHandler fn) {
        if (!raw_ || !raw_->register_hook || !fn) return false;
        auto holder = std::make_unique<HookHolder>(HookHolder{std::move(fn)});
        HookHolder* ptr = holder.get();
        const int rc = raw_->register_hook(raw_->ctx, detail::toMioStr(hookPoint),
                                           priority, &Host::hookThunk, ptr);
        if (rc == 0) {
            hooks_.push_back(std::move(holder));
            return true;
        }
        return false;
    }

    nlohmann::json pluginsJson() const {
        if (!raw_ || !raw_->plugins_json) return nlohmann::json::array();
        std::string out;
        raw_->plugins_json(raw_->ctx, &out, &detail::stringSinkWrite);
        try {
            if (!out.empty()) return nlohmann::json::parse(out);
        } catch (...) {
        }
        return nlohmann::json::array();
    }

    bool reloadPlugin(std::string_view pluginId = "") const {
        if (!raw_ || !raw_->reload_plugin) return false;
        return raw_->reload_plugin(raw_->ctx, detail::toMioStr(pluginId)) == 0;
    }

    const mio_host_api* raw() const { return raw_; }

private:
    struct ToolHolder {
        std::string name;
        ToolHandler fn;
    };

    struct HookHolder {
        HookHandler fn;
    };

    static int hookThunk(void* user, mio_str payload_json, void* sink, mio_write_fn write) noexcept {
        if (!user) return -1;
        try {
            auto* holder = static_cast<HookHolder*>(user);
            nlohmann::json payload = nlohmann::json::object();
            if (payload_json.data && payload_json.size > 0) {
                payload = nlohmann::json::parse(payload_json.data, payload_json.data + payload_json.size);
            }
            bool drop = false;
            auto modified = holder->fn(payload, drop);
            if (drop) {
                return 1;
            }
            if (modified.has_value() && write) {
                const std::string dumped = modified->dump();
                write(sink, detail::toMioStr(dumped));
            }
            return 0;
        } catch (...) {
            return -1;
        }
    }

    static int toolThunk(void* user, mio_str args_json, void* sink, mio_write_fn write) noexcept {
        if (!user) return -1;
        try {
            auto* holder = static_cast<ToolHolder*>(user);
            nlohmann::json args = nlohmann::json::object();
            if (args_json.data && args_json.size > 0) {
                args = nlohmann::json::parse(args_json.data, args_json.data + args_json.size);
            }
            const std::string res = holder->fn(args);
            if (write && !res.empty()) {
                write(sink, detail::toMioStr(res));
            }
            return 0;
        } catch (const std::exception& e) {
            const std::string err = std::string("error: ") + e.what();
            if (write) write(sink, detail::toMioStr(err));
            return -1;
        } catch (...) {
            const std::string err = "error: unknown plugin tool exception";
            if (write) write(sink, detail::toMioStr(err));
            return -1;
        }
    }

    static int channelSendThunk(void* user, mio_str outbound_json, void* sink, mio_write_fn write) noexcept {
        if (!user) return -1;
        try {
            auto* ch = static_cast<Channel*>(user);
            nlohmann::json outbound = nlohmann::json::object();
            if (outbound_json.data && outbound_json.size > 0) {
                outbound = nlohmann::json::parse(outbound_json.data, outbound_json.data + outbound_json.size);
            }
            const bool ok = ch->send(outbound);
            return ok ? 0 : -1;
        } catch (const std::exception& e) {
            const std::string err = e.what();
            if (write && !err.empty()) write(sink, detail::toMioStr(err));
            return -1;
        } catch (...) {
            return -1;
        }
    }

    static void channelCapThunk(void* user, void* sink, mio_write_fn write) noexcept {
        if (!user || !write) return;
        try {
            auto* ch = static_cast<Channel*>(user);
            const std::string cap = ch->capabilities().dump();
            write(sink, detail::toMioStr(cap));
        } catch (...) {
        }
    }

    const mio_host_api* raw_ = nullptr;
    std::vector<std::unique_ptr<ToolHolder>> tools_;
    std::vector<std::unique_ptr<HookHolder>> hooks_;
};

class Plugin {
public:
    virtual ~Plugin() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool reconfigure(const nlohmann::json& /*cfg*/) { return false; }
};

// 静态链接模式下的全局插件注册表
inline std::vector<const mio_plugin_desc*>& staticPluginRegistry() {
    static std::vector<const mio_plugin_desc*> registry;
    return registry;
}

inline void registerStaticPlugin(const mio_plugin_desc* desc) {
    if (!desc) return;
    auto& reg = staticPluginRegistry();
    for (const auto* existing : reg) {
        if (existing == desc || (existing->id && desc->id && std::string_view(existing->id) == desc->id)) {
            return;
        }
    }
    reg.push_back(desc);
}

namespace detail {

template <typename PluginClass>
struct PluginInstanceWrapper {
    Host host;
    std::unique_ptr<PluginClass> instance;

    explicit PluginInstanceWrapper(const mio_host_api* api) : host(api) {}
};

template <typename PluginClass>
void* createThunk(const mio_host_api* host, mio_str config_json) noexcept {
    try {
        nlohmann::json cfg = nlohmann::json::object();
        if (config_json.data && config_json.size > 0) {
            cfg = nlohmann::json::parse(config_json.data, config_json.data + config_json.size);
        }
        auto wrapper = std::make_unique<PluginInstanceWrapper<PluginClass>>(host);
        wrapper->instance = std::make_unique<PluginClass>(wrapper->host, cfg);
        return wrapper.release();
    } catch (const std::exception& e) {
        if (host && host->log) {
            host->log(host->ctx, MIO_LOG_ERROR, toMioStr("PluginSDK"), toMioStr(e.what()));
        }
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

template <typename PluginClass>
int startThunk(void* self) noexcept {
    if (!self) return -1;
    try {
        auto* wrapper = static_cast<PluginInstanceWrapper<PluginClass>*>(self);
        return wrapper->instance->start() ? 0 : -1;
    } catch (const std::exception& e) {
        auto* wrapper = static_cast<PluginInstanceWrapper<PluginClass>*>(self);
        wrapper->host.error("PluginSDK", e.what());
        return -1;
    } catch (...) {
        return -1;
    }
}

template <typename PluginClass>
void stopThunk(void* self) noexcept {
    if (!self) return;
    try {
        auto* wrapper = static_cast<PluginInstanceWrapper<PluginClass>*>(self);
        wrapper->instance->stop();
    } catch (...) {
    }
}

template <typename PluginClass>
void destroyThunk(void* self) noexcept {
    if (!self) return;
    try {
        auto* wrapper = static_cast<PluginInstanceWrapper<PluginClass>*>(self);
        delete wrapper;
    } catch (...) {
    }
}

template <typename PluginClass>
int reconfigureThunk(void* self, mio_str config_json) noexcept {
    if (!self) return -1;
    try {
        auto* wrapper = static_cast<PluginInstanceWrapper<PluginClass>*>(self);
        nlohmann::json cfg = nlohmann::json::object();
        if (config_json.data && config_json.size > 0) {
            cfg = nlohmann::json::parse(config_json.data, config_json.data + config_json.size);
        }
        return wrapper->instance->reconfigure(cfg) ? 0 : 1;
    } catch (...) {
        return -1;
    }
}

} // namespace detail

} // namespace mio::sdk

#define MIO_PLUGIN_CONCAT_INNER(a, b) a##b
#define MIO_PLUGIN_CONCAT(a, b) MIO_PLUGIN_CONCAT_INNER(a, b)

// MIO_PLUGIN 宏：同时支持静态注册（编译进主程序）与动态导出符号
#if defined(MIO_PLUGIN_STATIC)
#define MIO_PLUGIN(PluginClass, IdStr, VersionStr)                              \
    namespace {                                                                 \
    const mio_plugin_desc MIO_PLUGIN_CONCAT(kMioPluginDesc_, __LINE__) = {      \
        static_cast<uint32_t>(sizeof(mio_plugin_desc)),                         \
        MIO_PLUGIN_ABI_VERSION,                                                 \
        IdStr,                                                                  \
        VersionStr,                                                             \
        &::mio::sdk::detail::createThunk<PluginClass>,                          \
        &::mio::sdk::detail::startThunk<PluginClass>,                           \
        &::mio::sdk::detail::stopThunk<PluginClass>,                            \
        &::mio::sdk::detail::destroyThunk<PluginClass>,                         \
        &::mio::sdk::detail::reconfigureThunk<PluginClass>                      \
    };                                                                          \
    struct MIO_PLUGIN_CONCAT(MioStaticPluginRegistrar_, __LINE__) {             \
        MIO_PLUGIN_CONCAT(MioStaticPluginRegistrar_, __LINE__)() {              \
            ::mio::sdk::registerStaticPlugin(                                   \
                &MIO_PLUGIN_CONCAT(kMioPluginDesc_, __LINE__));                 \
        }                                                                       \
    } MIO_PLUGIN_CONCAT(g_mioStaticPluginRegistrar_, __LINE__);                 \
    }
#else
#define MIO_PLUGIN(PluginClass, IdStr, VersionStr)                              \
    namespace {                                                                 \
    const mio_plugin_desc kMioPluginDesc = {                                    \
        static_cast<uint32_t>(sizeof(mio_plugin_desc)),                         \
        MIO_PLUGIN_ABI_VERSION,                                                 \
        IdStr,                                                                  \
        VersionStr,                                                             \
        &::mio::sdk::detail::createThunk<PluginClass>,                          \
        &::mio::sdk::detail::startThunk<PluginClass>,                           \
        &::mio::sdk::detail::stopThunk<PluginClass>,                            \
        &::mio::sdk::detail::destroyThunk<PluginClass>,                         \
        &::mio::sdk::detail::reconfigureThunk<PluginClass>                      \
    };                                                                          \
    }                                                                           \
    extern "C" MIO_EXPORT const mio_plugin_desc* mio_plugin_entry(void) {       \
        return &kMioPluginDesc;                                                 \
    }
#endif
