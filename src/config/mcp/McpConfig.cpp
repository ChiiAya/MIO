#include "config/mcp/McpConfig.h"

namespace mio {
namespace mcp {

namespace {

template <typename T>
void readIfExists(const nlohmann::json& j, const char* key, T& target) {
    if (j.contains(key) && !j[key].is_null()) {
        try {
            target = j[key].get<T>();
        } catch (...) {}
    }
}

} // namespace

void to_json(nlohmann::json& j, const McpServerConfig& c) {
    j = nlohmann::json{
        {"command", c.command},
        {"args", c.args},
        {"env", c.env},
        {"transport", c.transport},
        {"toolPrefix", c.toolPrefix},
        {"enabled", c.enabled},
        {"timeoutMs", c.timeoutMs}
    };
}

void from_json(const nlohmann::json& j, McpServerConfig& c) {
    readIfExists(j, "command", c.command);
    readIfExists(j, "args", c.args);
    readIfExists(j, "env", c.env);
    readIfExists(j, "transport", c.transport);
    readIfExists(j, "toolPrefix", c.toolPrefix);
    readIfExists(j, "enabled", c.enabled);
    readIfExists(j, "timeoutMs", c.timeoutMs);
}

void to_json(nlohmann::json& j, const McpConfig& c) {
    j = nlohmann::json{
        {"enabled", c.enabled},
        {"defaultTimeoutMs", c.defaultTimeoutMs},
        {"servers", c.servers}
    };
}

void from_json(const nlohmann::json& j, McpConfig& c) {
    readIfExists(j, "enabled", c.enabled);
    readIfExists(j, "defaultTimeoutMs", c.defaultTimeoutMs);
    readIfExists(j, "servers", c.servers);
}

} // namespace mcp
} // namespace mio
