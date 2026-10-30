#pragma once

#include "core/event/Event.h"
#include "core/message/Message.h"
#include "plugin/HostApi.h"
#include "plugin/OutboundRouter.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mio {

class Runtime;

class PluginManager {
public:
    explicit PluginManager(Runtime& runtime);
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // 加载所有已启用的静态插件
    void loadAll();
    void startAll();
    void stopAll();

    // 查询插件状态列表 / 热重载指定或全部插件（特别是 lua-host / lua:<id>）
    nlohmann::json listPluginsJson() const;
    bool reloadPlugin(const std::string& pluginId = "");

    // 供主线程阻塞等待，直到收到退出信号或插件调用 request_shutdown
    void waitForShutdown();
    void requestShutdown();
    bool isShuttingDown() const { return shutdownRequested_.load(); }

    // 供 HostApi 注册出站通道与发送消息
    bool registerChannel(const std::string& owner,
                         const std::string& source,
                         const mio_channel_vtbl* vt,
                         void* user);
    void unregisterChannelsByOwner(const std::string& owner);
    bool sendOutbound(const Outbound& outbound);

    // 供 HostApi 注册与执行 Hook 管线 ("on_event" | "on_before_llm" | "on_outbound")
    bool registerHook(const std::string& owner,
                      const std::string& hookPoint,
                      int priority,
                      mio_hook_fn fn,
                      void* user);
    void unregisterHooksByOwner(const std::string& owner);
    bool runEventHooks(Event& ev);
    void runBeforeLlmHooks(std::vector<Msg>& messages);
    bool runOutboundHooks(Outbound& outbound);

    OutboundRouter& outboundRouter() { return outboundRouter_; }
    const OutboundRouter& outboundRouter() const { return outboundRouter_; }
    Runtime& runtime() { return runtime_; }

private:
    struct LoadedPlugin {
        const mio_plugin_desc* desc = nullptr;
        std::unique_ptr<PluginHostContext> hostCtx;
        void* instance = nullptr;
        bool started = false;
        nlohmann::json config = nlohmann::json::object();
    };

    struct HookEntry {
        std::string owner;
        std::string hookPoint;
        int priority = 100;
        mio_hook_fn fn = nullptr;
        void* user = nullptr;
    };

    Runtime& runtime_;
    OutboundRouter outboundRouter_;
    std::vector<LoadedPlugin> plugins_;
    mutable std::mutex mtx_;

    std::map<std::string, std::vector<HookEntry>> hooks_;
    mutable std::mutex hookMtx_;

    std::atomic<bool> shutdownRequested_{false};
    std::mutex shutdownMtx_;
    std::condition_variable shutdownCv_;
};

} // namespace mio
