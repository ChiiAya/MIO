#pragma once

// ============================================================================
// MCP 配置结构定义
//
// 对应 config.json 中类似如下的配置：
// {
//   "mcp": {
//     "enabled": true,
//     "servers": {
//       "filesystem": {
//         "command": "npx",
//         "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
//         "enabled": true,
//         "toolPrefix": "fs_"
//       }
//     }
//   }
// }
// ============================================================================

#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mio {
namespace mcp {

struct McpServerConfig {
    std::string command;                       // 执行命令（如 "npx", "python3", "./server"）
    std::vector<std::string> args;             // 命令行参数
    std::map<std::string, std::string> env;    // 自定义环境变量
    std::string transport = "stdio";           // 传输协议：目前主要支持 "stdio"
    std::string toolPrefix;                    // 工具名前缀（如 "fs_"），为空则默认不加前缀
    bool enabled = true;                       // 是否启用
    int timeoutMs = 30000;                     // 单次调用超时（毫秒）
};

struct McpConfig {
    bool enabled = true;
    int defaultTimeoutMs = 30000;
    std::map<std::string, McpServerConfig> servers;
};

void to_json(nlohmann::json& j, const McpServerConfig& c);
void from_json(const nlohmann::json& j, McpServerConfig& c);

void to_json(nlohmann::json& j, const McpConfig& c);
void from_json(const nlohmann::json& j, McpConfig& c);

} // namespace mcp
} // namespace mio
