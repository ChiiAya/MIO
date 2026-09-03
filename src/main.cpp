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
#include "config/openai/OpenaiConfig.h"
#include "llm/Llm.h"
#include "llm/openai/OpenAiCompat.h"
#include "log/Log.h"
#include "runtime/Runtime.h"

namespace mio {
namespace {

// LLM 工厂：把 backend 名映射到具体实现。
// 目前只有 "openai"（Chat Completions 兼容协议）；未来接新协议族在此扩展。
std::unique_ptr<Llm> createLlm(const std::string& backend) {
    if (backend == "openai")
        return std::make_unique<OpenAiCompat>(OpenAiConfig::fromEnvironment());
    throw std::runtime_error("未知 LLM backend: " + backend);
}

} // namespace
} // namespace mio

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    try {
        mio::log::initFromEnvironment();

        const std::string backend = mio::llmBackendFromEnvironment();
        std::cout << "LLM backend: " << backend << "\n";

        mio::Runtime runtime("Mio", "data", mio::createLlm(backend));

        // 平台选择：MIO_PLATFORM=napcat 接入 NapCatQQ 反向 WebSocket
        const char* platform = std::getenv("MIO_PLATFORM");
        if (platform != nullptr && std::string(platform) == "napcat") {
            mio::runNapCat(runtime, mio::NapCatConfig::fromEnvironment());
        } else {
            mio::runConsole(runtime, std::cin, std::cout);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "启动失败: " << error.what() << "\n";
        return 1;
    }
}
