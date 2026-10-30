#include "providers/llm/tool/ToolRegistry.h"

#include <stdexcept>

namespace mio {

void ToolRegistry::add(ToolDef def, ToolHandler handler, ToolLayer layer, std::string owner) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (owner.empty()) {
        owner = (layer == ToolLayer::Builtin) ? "builtin" : "mcp";
    }
    std::string name = def.name;
    ToolEntry entry;
    entry.def = std::move(def);
    entry.handler = std::move(handler);
    entry.layer = layer;
    entry.owner = std::move(owner);
    entry.inflight = std::make_shared<std::atomic<int>>(0);
    tools_.insert_or_assign(name, std::move(entry));
}

bool ToolRegistry::remove(std::string_view name, std::string_view expectedOwner) {
    std::shared_ptr<std::atomic<int>> inflight;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = tools_.find(std::string(name));
        if (it == tools_.end()) return false;
        if (!expectedOwner.empty() && it->second.owner != expectedOwner) {
            return false;
        }
        inflight = it->second.inflight;
        tools_.erase(it);
    }
    if (inflight) {
        std::unique_lock<std::mutex> lock(mtx_);
        inflightCv_.wait(lock, [&] { return inflight->load() <= 0; });
    }
    return true;
}

std::size_t ToolRegistry::removeByOwner(std::string_view owner) {
    std::vector<std::shared_ptr<std::atomic<int>>> inflights;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = tools_.begin(); it != tools_.end(); ) {
            if (it->second.owner == owner) {
                if (it->second.inflight) {
                    inflights.push_back(it->second.inflight);
                }
                it = tools_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!inflights.empty()) {
        std::unique_lock<std::mutex> lock(mtx_);
        inflightCv_.wait(lock, [&] {
            for (const auto& counter : inflights) {
                if (counter->load() > 0) return false;
            }
            return true;
        });
    }
    return inflights.size();
}

void ToolRegistry::clearDynamicTools() {
    removeByOwner("mcp");
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
    std::shared_ptr<std::atomic<int>> inflight;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = tools_.find(name);
        if (it == tools_.end())
            throw std::out_of_range("工具不存在: " + name);
        handler = it->second.handler;
        inflight = it->second.inflight;
        if (inflight) {
            inflight->fetch_add(1);
        }
    }

    struct InflightGuard {
        const ToolRegistry* self;
        std::shared_ptr<std::atomic<int>> counter;
        ~InflightGuard() {
            if (counter && counter->fetch_sub(1) == 1) {
                std::lock_guard<std::mutex> lock(self->mtx_);
                self->inflightCv_.notify_all();
            }
        }
    } guard{this, inflight};

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

std::vector<std::string> ToolRegistry::namesByOwner(std::string_view owner) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::string> out;
    for (const auto& [name, entry] : tools_) {
        if (entry.owner == owner) out.push_back(name);
    }
    return out;
}

} // namespace mio
