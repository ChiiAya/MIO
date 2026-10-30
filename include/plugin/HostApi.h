#pragma once

#include "mio/plugin_abi.h"

#include <filesystem>
#include <string>

namespace mio {

class Runtime;
class PluginManager;

struct PluginHostContext {
    std::string pluginId;
    std::string owner; // "plugin:<id>"
    std::filesystem::path dataDir;
    Runtime* runtime = nullptr;
    PluginManager* manager = nullptr;
    mio_host_api api{};
};

void initHostApi(PluginHostContext& ctx);

} // namespace mio
