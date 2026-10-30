#pragma once

#include "core/message/Message.h"
#include "plugin/HostApi.h"

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

    Runtime& runtime() { return runtime_; }

private:
    struct LoadedPlugin {
        const mio_plugin_desc* desc = nullptr;
        std::unique_ptr<PluginHostContext> hostCtx;
        void* instance = nullptr;
        bool started = false;
    };

    struct ChannelBinding {
        std::string owner;
        std::string source;
        mio_channel_vtbl vtbl{};
        void* user = nullptr;
    };

    Runtime& runtime_;
    std::vector<LoadedPlugin> plugins_;
    mutable std::mutex mtx_;

    std::map<std::string, ChannelBinding> channels_;
    mutable std::mutex channelMtx_;

    std::atomic<bool> shutdownRequested_{false};
    std::mutex shutdownMtx_;
    std::condition_variable shutdownCv_;
};

} // namespace mio
