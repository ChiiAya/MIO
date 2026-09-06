#include "llm/tool/ToolRegistry.h"

namespace mio {

void ToolRegistry::add(ToolDef def, ToolHandler handler, ToolLayer layer) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string name = def.name;
    tools_.insert_or_assign(name, ToolEntry{std::move(def), std::move(handler), layer});
}

void ToolRegistry::clearDynamicTools() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = tools_.begin(); it != tools_.end(); ) {
        if (it->second.layer == ToolLayer::Dynamic) {
            it = tools_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<ToolDef> ToolRegistry::find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = tools_.find(name);
    if (it == tools_.end()) return std::nullopt;
    return it->second.def;
}

std::string ToolRegistry::invoke(const std::string& name,
                                 const nlohmann::json& args) const {
    ToolHandler handler;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = tools_.find(name);
        if (it == tools_.end())
            throw std::out_of_range("工具不存在: " + name);
        handler = it->second.handler;
    }
    return handler(args);
}

std::vector<ToolDef> ToolRegistry::defs() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<ToolDef> out;
    out.reserve(tools_.size());
    for (const auto& [_, entry] : tools_) out.push_back(entry.def);
    return out;
}

std::vector<std::string> ToolRegistry::names() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> out;
    out.reserve(tools_.size());
    for (const auto& [name, _] : tools_) out.push_back(name);
    return out;
}

} // namespace mio
