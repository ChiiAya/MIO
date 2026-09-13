// MemoryProviderFactory 实现（见 providers/memory/MemoryProviderFactory.h）

#include "providers/memory/MemoryProviderFactory.h"

#include "log/Log.h"
#include "providers/memory/HindsightMemoryProvider.h"
#include "providers/memory/LocalMemoryProvider.h"
#include "providers/memory/UnavailableMemoryProvider.h"

#include <cctype>

namespace mio {

namespace {

// 归一化：去掉首尾空白并转小写，仅用于名称比较（不改动配置原文）。
std::string normalizeBackend(const std::string& backend) {
    std::size_t b = 0;
    std::size_t e = backend.size();
    while (b < e && std::isspace(static_cast<unsigned char>(backend[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(backend[e - 1])) != 0) --e;
    std::string out;
    out.reserve(e - b);
    for (std::size_t i = b; i < e; ++i)
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(backend[i]))));
    return out;
}

} // namespace

std::shared_ptr<MemoryProvider> makeMemoryProvider(const std::string& backend,
                                                   MemoryManager& memory,
                                                   const MemoryConfig& config) {
    const std::string name = normalizeBackend(backend);
    if (name.empty() || name == "sqlite-local") {
        return std::make_shared<LocalMemoryProvider>(memory, config);
    }
    if (name == "unavailable") {
        return std::make_shared<UnavailableMemoryProvider>(
            "配置指定后端为 unavailable（无长期召回）");
    }
    if (name == "hindsight") {
        // 占位：HINDSIGHT_ADAPTER_TODO（配置/部署/联网/SDK 均未实现）
        log::warn("MemoryProviderFactory",
                  std::string(kHindsightAdapterTodo) +
                      "：Hindsight 后端未接入，降级为不可用（无长期召回）");
        return std::make_shared<HindsightMemoryProvider>();
    }
    log::warn("MemoryProviderFactory", "未知记忆后端名称 \"" + backend +
                                           "\"，降级为 unavailable（无长期召回）");
    return std::make_shared<UnavailableMemoryProvider>("未知记忆后端: " + backend);
}

} // namespace mio
