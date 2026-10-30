#pragma once

// ============================================================================
// McpManager —— 多 MCP Server 容器与 MIO ToolRegistry 动态注入桥梁
//
// 职责：
//   1. 管理多个配置的 McpClient 实例的生命周期（启动、重载、安全退出）；
//   2. 自动处理命名空间冲突与前缀映射；
//   3. 自动将 MCP Server 的工具转换为 MIO 的 ToolDef，并注册为 ToolLayer::Dynamic；
//   4. 供 Runtime 在冷启动与 reloadConfig 时调用。
// ============================================================================

#include "config/mcp/McpConfig.h"
#include "providers/mcp/McpClient.h"
#include "providers/llm/tool/ToolRegistry.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mio {
namespace mcp {

class McpManager {
public:
    McpManager() = default;
    ~McpManager();

    // 禁用拷贝与移动
    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;
    McpManager(McpManager&&) = delete;
    McpManager& operator=(McpManager&&) = delete;

    // 根据配置启动所有启用的 MCP Server 并拉取工具
    void initialize(const McpConfig& config);

    // 将所有已连接的 MCP 工具注册进 MIO 的 ToolRegistry
    void syncTools(ToolRegistry& registry);

    // 热重载：安全停止老连接，根据新配置重新启动并挂载工具
    void reload(const McpConfig& config, ToolRegistry& registry);

    // 停止并回收所有子进程与连接
    void stop();

    // 当前已成功连接的 Server 列表
    std::vector<std::string> activeServerNames() const;

private:
    std::map<std::string, std::shared_ptr<McpClient>> clients_;
    std::map<std::string, McpServerConfig> serverConfigs_;
    mutable std::mutex mtx_;
};

} // namespace mcp
} // namespace mio
