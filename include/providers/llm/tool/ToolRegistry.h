#pragma once
// ============================================================================
// 工具注册表（支持按 owner 分组管理与在途调用计数保护）
//
// owner 约定：
//   - "builtin"      : 系统核心内置工具
//   - "mcp"          : MCP 动态挂载工具
//   - "plugin:<id>"  : 原生插件注册工具
//   - "lua:<id>"     : Lua 轻量插件注册工具
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "providers/llm/Llm.h"

namespace mio {

enum class ToolLayer {
    Builtin,
    Dynamic
};

using ToolHandler = std::function<std::string(const nlohmann::json& args)>;

struct ToolEntry {
    ToolDef def;
    ToolHandler handler;
    ToolLayer layer = ToolLayer::Builtin;
    std::string owner = "builtin";
    std::shared_ptr<std::atomic<int>> inflight = std::make_shared<std::atomic<int>>(0);
};

class ToolRegistry {
public:
    void add(ToolDef def,
             ToolHandler handler,
             ToolLayer layer = ToolLayer::Builtin,
             std::string owner = "");

    // 按工具名移除（若指定 owner 非空，仅当归属匹配时移除），并等待在途调用归零
    bool remove(std::string_view name, std::string_view expectedOwner = "");

    // 按 owner 批量移除工具（如 "mcp"、"plugin:napcat"、"lua:dice"），并等待在途调用归零
    std::size_t removeByOwner(std::string_view owner);

    // 兼容旧接口：仅清理 owner == "mcp" 或未指定插件归属的 Dynamic 工具
    void clearDynamicTools();

    std::optional<ToolDef> find(const std::string& name) const;

    // 调用 handler；未知工具名抛 std::out_of_range
    std::string invoke(const std::string& name, const nlohmann::json& args) const;

    std::vector<ToolDef> defs() const;
    std::vector<std::string> names() const;
    std::vector<std::string> namesByOwner(std::string_view owner) const;

private:
    std::map<std::string, ToolEntry> tools_;
    mutable std::mutex mtx_;
    mutable std::condition_variable inflightCv_;
};

} // namespace mio
