// ============================================================================
// MIO 入口：基础设施组装（Runtime） + 插件管理器（PluginManager）
//
// main 只做三件事：
//   1. 初始化日志与加载配置；
//   2. 构造 Runtime（组合根：持有 LLM/身份簿/日记/存档/注意力门/意识流循环）；
//   3. 通过 PluginManager 加载并启动所有已启用插件，主线程等待退出信号后统一停止。
// ============================================================================

#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "config/ConfigManager.h"
#include "config/openai/OpenaiConfig.h"
#include "log/Log.h"
#include "plugin/PluginManager.h"
#include "providers/llm/Llm.h"
#include "providers/llm/openai/OpenAiCompat.h"
#include "runtime/Runtime.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace mio {
namespace {

PluginManager* g_pluginMgr = nullptr;

void signalHandler(int /*sig*/) {
    if (g_pluginMgr) {
        g_pluginMgr->requestShutdown();
    }
}

std::shared_ptr<Llm> createLlm(const AppConfig& cfg) {
    if (cfg.llmBackend == "openai")
        return std::make_shared<OpenAiCompat>(cfg.openai);
    throw std::runtime_error("未知 LLM backend: " + cfg.llmBackend);
}

} // namespace
} // namespace mio

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    try {
        mio::log::initFromEnvironment();

        std::filesystem::path configPath;
        if (const char* envPath = std::getenv("MIO_CONFIG_PATH")) {
            configPath = envPath;
        }

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--config" || arg == "-c") {
                if (i + 1 < argc) {
                    configPath = argv[++i];
                }
            } else if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: " << argv[0] << " [config_file] [--config <path>]\n";
                std::cout << "Options:\n";
                std::cout << "  -c, --config <path>   指定配置文件路径 (默认: config.json 或 $MIO_CONFIG_PATH)\n";
                std::cout << "  -h, --help            显示帮助信息\n";
                return 0;
            } else if (!arg.empty() && arg[0] != '-') {
                configPath = arg;
            }
        }

        std::shared_ptr<mio::ConfigManager> configMgr;
        if (!configPath.empty()) {
            configMgr = std::make_shared<mio::ConfigManager>(configPath);
        } else {
            configMgr = std::make_shared<mio::ConfigManager>();
        }

        auto cfg = configMgr->get();
        std::cout << "Bot Name: " << cfg->botName << "\n";
        std::cout << "LLM backend: " << cfg->llmBackend;
        if (!cfg->openai.model.empty()) {
            std::cout << " (" << cfg->openai.model << ")";
        }
        std::cout << "\n";

        mio::Runtime runtime(cfg->botName, cfg->dataDir, mio::createLlm(*cfg), configMgr);
        mio::PluginManager plugins(runtime);
        mio::g_pluginMgr = &plugins;

        std::signal(SIGINT, mio::signalHandler);
        std::signal(SIGTERM, mio::signalHandler);

        plugins.loadAll();
        plugins.startAll();
        plugins.waitForShutdown();
        plugins.stopAll();

        mio::g_pluginMgr = nullptr;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "启动失败: " << error.what() << "\n";
        return 1;
    }
}
