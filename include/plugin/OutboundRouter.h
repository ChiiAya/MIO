#pragma once
// ============================================================================
// OutboundRouter —— 出站消息路由器（按 source 分发到对应插件通道 + [SEP] 拆气泡）
// ============================================================================

#include "config/AppConfig.h"
#include "core/message/Message.h"
#include "mio/plugin_abi.h"

#include <map>
#include <mutex>
#include <string>

namespace mio {

class OutboundRouter {
public:
    bool registerChannel(const std::string& owner,
                         const std::string& source,
                         const mio_channel_vtbl* vt,
                         void* user);
    void unregisterByOwner(const std::string& owner);
    bool hasAnyChannel() const;

    // 检查文本去掉空白与分段符后是否仍有可见内容
    static bool hasDeliverableText(const std::string& text, const MessageSplitterConfig& splitter);

    // 拆分 [SEP] 气泡、执行打字节奏延迟，并按 source 路由到目标通道发送
    bool emit(const Outbound& outbound, const MessageSplitterConfig& splitter) const;

    // 直接发送单个气泡到目标通道
    bool sendBubble(const Outbound& bubble) const;

private:
    struct ChannelBinding {
        std::string owner;
        std::string source;
        mio_channel_vtbl vtbl{};
        void* user = nullptr;
    };

    std::map<std::string, ChannelBinding> channels_;
    mutable std::mutex mtx_;
};

} // namespace mio
