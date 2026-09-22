#include "providers/mcp/McpManager.h"
#include "providers/mcp/transport/ProcessTransport.h"
#include "log/Log.h"

namespace mio {
namespace mcp {

McpManager::~McpManager() {
    stop();
}

void McpManager::initialize(const McpConfig& config) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!config.enabled) {
        log::info("McpManager", "MCP 功能未启用 (enabled=false)");
        return;
    }

    for (const auto& [serverName, srvCfg] : config.servers) {
        if (!srvCfg.enabled) continue;

        std::shared_ptr<ITransport> transport;
        if (srvCfg.transport == "stdio") {
            transport = std::make_shared<ProcessTransport>(
                srvCfg.command, srvCfg.args, srvCfg.env);
        } else {
            log::warn("McpManager", "暂不支持的 MCP 传输协议: " + srvCfg.transport
                      + " (Server: " + serverName + ")");
            continue;
        }

        const int timeoutMs = srvCfg.timeoutMs > 0 ? srvCfg.timeoutMs : config.defaultTimeoutMs;
        auto client = std::make_shared<McpClient>(serverName, transport, timeoutMs);

        log::info("McpManager", "正在连接 MCP 服务: " + serverName
                  + " (命令: " + srvCfg.command + ")");

        if (client->connect()) {
            clients_[serverName] = client;
            serverConfigs_[serverName] = srvCfg;
            log::info("McpManager", "MCP 服务连接成功: " + serverName);
        } else {
            log::error("McpManager", "MCP 服务连接失败: " + serverName);
        }
    }
}

void McpManager::syncTools(ToolRegistry& registry) {
    std::lock_guard<std::mutex> lock(mtx_);

    for (const auto& [serverName, client] : clients_) {
        try {
            // 显式拷贝，避免在 C++17 下捕获结构化绑定（C++20 才允许）
            auto clientPtr = client;
            std::vector<McpTool> tools = client->listTools();
            const auto& srvCfg = serverConfigs_[serverName];

            for (const auto& tool : tools) {
                std::string registeredName;
                if (!srvCfg.toolPrefix.empty()) {
                    registeredName = srvCfg.toolPrefix + tool.name;
                } else {
                    // 若重名且未配置前缀，自动加 serverName_ 前缀隔离
                    if (registry.find(tool.name).has_value()) {
                        registeredName = serverName + "_" + tool.name;
                        log::warn("McpManager", "工具名 " + tool.name
                                  + " 已存在，自动重命名为 " + registeredName);
                    } else {
                        registeredName = tool.name;
                    }
                }

                ToolDef def;
                def.name = registeredName;
                def.description = "[MCP:" + serverName + "] " + tool.description;
                def.parametersJsonSchema = tool.inputSchema;
                def.isTerminal = false;

                auto originalName = tool.name;
                ToolHandler handler = [clientPtr, originalName](const nlohmann::json& args) -> std::string {
                    return clientPtr->callTool(originalName, args);
                };

                // 以 Dynamic 动态层注入注册表
                registry.add(std::move(def), std::move(handler), ToolLayer::Dynamic);
                log::info("McpManager", "成功挂载 MCP 动态工具: " + registeredName);
            }
        } catch (const std::exception& e) {
            log::error("McpManager", "获取 Server [" + serverName
                      + "] 工具列表异常: " + std::string(e.what()));
        }
    }
}

void McpManager::reload(const McpConfig& config, ToolRegistry& registry) {
    stop();
    registry.clearDynamicTools();
    initialize(config);
    syncTools(registry);
}

void McpManager::stop() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [name, client] : clients_) {
        log::info("McpManager", "正在关闭 MCP 服务: " + name);
        client->disconnect();
    }
    clients_.clear();
    serverConfigs_.clear();
}

std::vector<std::string> McpManager::activeServerNames() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> names;
    for (const auto& [name, _] : clients_) {
        names.push_back(name);
    }
    return names;
}

} // namespace mcp
} // namespace mio
