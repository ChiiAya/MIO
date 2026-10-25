#pragma once

// ============================================================================
// McpClient —— 单个 MCP 服务连接协议客户端
//
// 职责：
//   1. 驱动与单个 MCP Server 的完整交互生命周期；
//   2. 实现协议握手（initialize -> notifications/initialized）；
//   3. 工具发现（tools/list）与工具远程调用（tools/call）；
//   4. 采用 std::promise / std::future 实现基于消息 ID 的异步匹配与超时保护。
// ============================================================================

#include "providers/mcp/protocol/JsonRpc.h"
#include "providers/mcp/transport/ITransport.h"

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mio {
namespace mcp {

// 从 MCP Server 发现的标准工具数据结构
struct McpTool {
    std::string name;
    std::string description;
    nlohmann::json inputSchema; // 符合 JSON Schema 标准的参数对象定义
};

class McpClient : public std::enable_shared_from_this<McpClient> {
public:
    McpClient(std::string name,
              std::shared_ptr<ITransport> transport,
              int defaultTimeoutMs = 30000);
    ~McpClient();

    // 禁用拷贝，支持移动或 shared_ptr 托管
    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    const std::string& name() const { return name_; }

    // 发起连接与协议握手协商
    bool connect();

    // 关闭连接
    void disconnect();

    // 检查连接是否可用
    bool isConnected() const;

    // 拉取服务端声明的所有工具
    std::vector<McpTool> listTools();

    // 调用指定工具并同步等待返回文本结果
    std::string callTool(const std::string& toolName, const nlohmann::json& arguments);

private:
    // 发送带有超时等待的 JSON-RPC 请求
    nlohmann::json sendRequest(const std::string& method,
                               const nlohmann::json& params,
                               int timeoutMs = -1);

    // 发送单向通知（无需响应）
    bool sendNotification(const std::string& method,
                          const nlohmann::json& params = nlohmann::json::object());

    // 传输层收到消息时的派发逻辑
    void onMessage(const nlohmann::json& msg);
    void onError(const std::string& errorMsg);

    std::string name_;
    std::shared_ptr<ITransport> transport_;
    int defaultTimeoutMs_;

    std::atomic<bool> connected_{false};
    std::atomic<std::int64_t> nextId_{1};

    // 异步回调 Promise 表（基于请求 ID 映射）
    mutable std::mutex pendingMtx_;
    std::map<std::int64_t, std::shared_ptr<std::promise<nlohmann::json>>> pendingRequests_;
};

} // namespace mcp
} // namespace mio
