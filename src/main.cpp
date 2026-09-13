// ============================================================================
// MIO 入口：基础设施组装（Runtime） + 平台适配器（Console / NapCat）
//
// main 只做三件事：
//   1. 初始化日志（log::initFromEnvironment，等级读 MIO_LOG_LEVEL）；
//   2. 构造 Runtime（组合根：持有 LLM/身份簿/日记/存档/装配器/工具表）；
//   3. 按平台进入事件循环：MIO_PLATFORM=napcat → runNapCat（NapCatQQ），
//      默认 → runConsole（控制台演示）—— 平台差异都在 adapters/。
//
// LLM 由工厂创建（llmBackendFromEnvironment 选协议族，目前仅 "openai"）；
// 也可以直接 Runtime runtime("Mio")，走环境变量构造默认实现。
//
// 运行前置：
//   export MIO_BASE_URL=http://127.0.0.1:8099   # 指向 OpenAI 兼容端点
//   export MIO_API_KEY=dummy
//   export MIO_MODEL=fake-mini
//   python3 scripts/fake_openai_server.py       # 先起假服务器联调
// ============================================================================

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "adapters/console/ConsoleAdapter.h"
#include "adapters/napcat/NapCatAdapter.h"
#include "config/ConfigManager.h"
#include "config/openai/OpenaiConfig.h"
#include "providers/llm/Llm.h"
#include "providers/llm/openai/OpenAiCompat.h"
#include "log/Log.h"
#include "runtime/Runtime.h"

namespace mio {
namespace {

// LLM 工厂：根据配置把 backend 映射到具体实现。
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

        // 平台选择：MIO_PLATFORM 环境变量优先，未设置则使用配置文件里的 platform
        std::string platform = cfg->platform;
        if (const char* envPlatform = std::getenv("MIO_PLATFORM")) {
            platform = envPlatform;
        }

        if (platform == "napcat") {
            mio::runNapCat(runtime, cfg->napcat);
        } else {
            mio::runConsole(runtime, std::cin, std::cout);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "启动失败: " << error.what() << "\n";
        return 1;
    }
}
