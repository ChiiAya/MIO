#pragma once

// ============================================================================
// MCP 传输层抽象接口 (ITransport)
//
// 规范：MCP 支持多种底层 Transport，包括：
//   1. Stdio (子进程管道)：最常见，用于本地拉起 CLI 工具（npx, python 等）。
//   2. SSE / Streamable HTTP：用于跨进程或远程网络服务。
//
// 本接口将网络/进程的具体通信细节（管道创建、套接字、事件轮询、线程读写）
// 与上层的 MCP 协议状态机完全解耦。
// ============================================================================

#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace mio {
namespace mcp {

class ITransport {
public:
    virtual ~ITransport() = default;

    // 启动传输通道（例如启动子进程或建立网络长连接）
    virtual bool start() = 0;

    // 停止传输通道并释放相关底层句柄与资源
    virtual void stop() = 0;

    // 当前通道是否处于活跃可用状态
    virtual bool isRunning() const = 0;

    // 向对端发送一个 JSON 报文（要求线程安全）
    virtual bool send(const nlohmann::json& message) = 0;

    // 注册收到完整 JSON 报文时的异步回调
    virtual void setOnMessage(std::function<void(const nlohmann::json&)> callback) = 0;

    // 注册通道发生致命错误或意外断开时的回调
    virtual void setOnError(std::function<void(const std::string&)> callback) = 0;
};
    
}
}
