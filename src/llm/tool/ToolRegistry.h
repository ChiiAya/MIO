#pragma once
// ============================================================================
// 工具注册表
//
// 同名注册时新条目覆盖旧条目。
// invoke 的异常约定：handler 抛出的任何异常由调用方捕获后转成
// "error: ..." 文本回喂模型（自愈原则），未知工具名/无 handler 直接抛。
// ============================================================================

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "llm/Llm.h"

namespace mio {

using ToolHandler = std::function<std::string(const nlohmann::json& args)>;

class ToolRegistry {
public:
    void add(const ToolDef& def, ToolHandler handler);

    const ToolDef* find(const std::string& name) const;

    // 调用 handler；未知工具名抛 std::out_of_range，
    // handler 自身的异常原样上抛（由 ToolLoop 捕获转换）
    std::string invoke(const std::string& name, const nlohmann::json& args) const;

    std::vector<ToolDef> defs() const;
    std::vector<std::string> names() const;

private:
    std::map<std::string, std::pair<ToolDef, ToolHandler>> tools_;
};

} // namespace mio
