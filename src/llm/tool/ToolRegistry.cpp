#include "llm/tool/ToolRegistry.h"

namespace mio {

void ToolRegistry::add(const ToolDef& def, ToolHandler handler) {
    tools_.insert_or_assign(def.name,
                            std::make_pair(def, std::move(handler)));
}

const ToolDef* ToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : &it->second.first;
}

std::string ToolRegistry::invoke(const std::string& name,
                                 const nlohmann::json& args) const {
    auto it = tools_.find(name);
    if (it == tools_.end())
        throw std::out_of_range("工具不存在: " + name);
    return it->second.second(args);
}

std::vector<ToolDef> ToolRegistry::defs() const {
    std::vector<ToolDef> out;
    out.reserve(tools_.size());
    for (const auto& [_, pair] : tools_) out.push_back(pair.first);
    return out;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& [name, _] : tools_) out.push_back(name);
    return out;
}

} // namespace mio
