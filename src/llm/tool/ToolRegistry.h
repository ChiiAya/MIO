#pragma once
// ============================================================================
// 工具注册表
//
// 同名注册时新条目覆盖旧条目。
// 支持分层管理：系统核心内置工具（Builtin）与动态扩展工具（Dynamic）。
// invoke 的异常约定：handler 抛出的任何异常由调用方捕获后转成
// "error: ..." 文本回喂模型（自愈原则），未知工具名/无 handler 直接抛。
// ============================================================================

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/Llm.h"

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
};

class ToolRegistry {
public:
    void add(ToolDef def, ToolHandler handler, ToolLayer layer = ToolLayer::Builtin);

    // 清理所有动态注册的工具，保留内置工具
    void clearDynamicTools();

    std::optional<ToolDef> find(const std::string& name) const;

    // 调用 handler；未知工具名抛 std::out_of_range，
    // handler 自身的异常原样上抛（由 ToolLoop 捕获转换）
    std::string invoke(const std::string& name, const nlohmann::json& args) const;

    std::vector<ToolDef> defs() const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, ToolEntry> tools_;
    mutable std::mutex mtx_;
};

} // namespace mio
