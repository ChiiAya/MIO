#include "providers/mcp/McpClient.h"
#include "providers/mcp/transport/ProcessTransport.h"
#include "providers/mcp/transport/ITransport.h"
#include "log/Log.h"
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace mio {
namespace mcp {

McpClient::McpClient(std::string name,
                     std::shared_ptr<ITransport> transport,
                     int defaultTimeoutMs)
    : name_(std::move(name)),
      transport_(std::move(transport)),
      defaultTimeoutMs_(defaultTimeoutMs) {}

McpClient::~McpClient() {
    disconnect();
}

bool McpClient::connect() {
    if (!transport_) return false;

    // 绑定传输层事件回调
    transport_->setOnMessage([this](const nlohmann::json& msg) {
        onMessage(msg);
    });

    transport_->setOnError([this](const std::string& err) {
        onError(err);
    });

    if (!transport_->start()) {
        return false;
    }

    // 1. 发起 initialize 协议握手
    nlohmann::json initParams = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", {
            {"name", "Mio"},
            {"version", "1.0.0"}
        }}
    };

    try {
        nlohmann::json initResult = sendRequest("initialize", initParams, 10000);
        // 2. 发送握手确认通知 (notifications/initialized)
        sendNotification("notifications/initialized");
        connected_.store(true);
        return true;
    } catch (const std::exception& e) {
        log::error("McpClient", "[" + name_ + "] 协议握手失败: " + e.what());
        disconnect();
        return false;
    }
}

void McpClient::disconnect() {
    // 幂等且与连接状态无关：传输层可能已经因为子进程退出而失效
    // （此时 isRunning() 已是 false），但 stop() 仍需要被调用一次来
    // 回收子进程与读线程。在途请求也必须无条件唤醒，避免永久挂起。
    connected_.store(false);

    {
        std::lock_guard<std::mutex> lock(pendingMtx_);
        for (auto& [id, prom] : pendingRequests_) {
            try {
                prom->set_exception(std::make_exception_ptr(
                    std::runtime_error("MCP 客户端连接已关闭")));
            } catch (...) {}
        }
        pendingRequests_.clear();
    }

    if (transport_) {
        transport_->stop();
    }
}

bool McpClient::isConnected() const {
    return connected_.load() && transport_ && transport_->isRunning();
}

std::vector<McpTool> McpClient::listTools() {
    if (!isConnected()) {
        throw std::runtime_error("MCP 客户端未连接，无法查询工具列表");
    }

    nlohmann::json resp = sendRequest("tools/list", nlohmann::json::object());
    std::vector<McpTool> out;

    if (resp.contains("tools") && resp["tools"].is_array()) {
        for (const auto& t : resp["tools"]) {
            McpTool tool;
            tool.name = t.value("name", "");
            tool.description = t.value("description", "");
            if (t.contains("inputSchema") && t["inputSchema"].is_object()) {
                tool.inputSchema = t["inputSchema"];
            } else {
                tool.inputSchema = {{"type", "object"}, {"properties", nlohmann::json::object()}};
            }
            if (!tool.name.empty()) {
                out.push_back(std::move(tool));
            }
        }
    }
    return out;
}

std::string McpClient::callTool(const std::string& toolName, const nlohmann::json& arguments) {
    if (!isConnected()) {
        throw std::runtime_error("MCP 客户端未连接，无法调用工具: " + toolName);
    }

    nlohmann::json params = {
        {"name", toolName},
        {"arguments", arguments.is_object() ? arguments : nlohmann::json::object()}
    };

    nlohmann::json result = sendRequest("tools/call", params);

    std::string textResult;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& item : result["content"]) {
            if (item.value("type", "") == "text") {
                if (!textResult.empty()) textResult += "\n";
                textResult += item.value("text", "");
            }
        }
    }

    if (result.value("isError", false)) {
        return "error: [MCP:" + name_ + "] " + (textResult.empty() ? "工具返回错误" : textResult);
    }

    return textResult.empty() ? "(空执行结果)" : textResult;
}

nlohmann::json McpClient::sendRequest(const std::string& method,
                                      const nlohmann::json& params,
                                      int timeoutMs) {
    if (!transport_ || !transport_->isRunning()) {
        throw std::runtime_error("传输通道已失效");
    }

    const int actualTimeout = (timeoutMs > 0) ? timeoutMs : defaultTimeoutMs_;
    const std::int64_t reqId = nextId_.fetch_add(1);

    auto prom = std::make_shared<std::promise<nlohmann::json>>();
    auto fut = prom->get_future();

    {
        std::lock_guard<std::mutex> lock(pendingMtx_);
        pendingRequests_[reqId] = prom;
    }

    JsonRpcRequest req;
    req.id = reqId;
    req.method = method;
    req.params = params;

    if (!transport_->send(req.toJson())) {
        std::lock_guard<std::mutex> lock(pendingMtx_);
        pendingRequests_.erase(reqId);
        throw std::runtime_error("向 MCP 服务端发送请求数据包失败: " + method);
    }

    // 等待响应并在超时时清理现场
    if (fut.wait_for(std::chrono::milliseconds(actualTimeout)) == std::future_status::timeout) {
        {
            std::lock_guard<std::mutex> lock(pendingMtx_);
            pendingRequests_.erase(reqId);
        }
        throw std::runtime_error("MCP 请求超时 (" + std::to_string(actualTimeout) + "ms): " + method);
    }

    nlohmann::json respJson = fut.get();
    JsonRpcResponse resp = JsonRpcResponse::fromJson(respJson);

    if (!resp.isSuccess()) {
        std::ostringstream oss;
        oss << "MCP 调用报错 (" << resp.error->code << "): " << resp.error->message;
        throw std::runtime_error(oss.str());
    }

    return resp.result.is_null() ? nlohmann::json::object() : resp.result;
}

bool McpClient::sendNotification(const std::string& method, const nlohmann::json& params) {
    if (!transport_ || !transport_->isRunning()) return false;

    JsonRpcNotification notif;
    notif.method = method;
    notif.params = params;
    return transport_->send(notif.toJson());
}

void McpClient::onMessage(const nlohmann::json& msg) {
    if (!msg.is_object()) return;

    // 1. 检查是否为对端发来的响应（携带 id）
    if (msg.contains("id") && msg["id"].is_number()) {
        const std::int64_t id = msg["id"].get<std::int64_t>();
        std::shared_ptr<std::promise<nlohmann::json>> prom;
        {
            std::lock_guard<std::mutex> lock(pendingMtx_);
            auto it = pendingRequests_.find(id);
            if (it != pendingRequests_.end()) {
                prom = it->second;
                pendingRequests_.erase(it);
            }
        }
        if (prom) {
            prom->set_value(msg);
        }
        return;
    }

    // 2. 服务端主动推送的请求或通知
    if (msg.contains("method")) {
        const std::string method = msg.value("method", "");
        // 处理服务端心跳 ping
        if (method == "ping" && msg.contains("id")) {
            nlohmann::json pong = {
                {"jsonrpc", "2.0"},
                {"id", msg["id"]},
                {"result", nlohmann::json::object()}
            };
            if (transport_) transport_->send(pong);
        }
    }
}

void McpClient::onError(const std::string& errorMsg) {
    log::warn("McpClient", "[" + name_ + "] 传输层通知错误: " + errorMsg);
    disconnect();
}

} // namespace mcp
} // namespace mio
