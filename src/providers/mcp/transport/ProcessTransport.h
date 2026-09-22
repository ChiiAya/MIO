#pragma once

// ============================================================================
// ProcessTransport —— 基于标准子进程管道 (Stdio) 的传输层实现
//
// 运作原理：
//   1. 父进程创建两条无名管道（stdinPipe 与 stdoutPipe）；
//   2. 通过 fork + execvp 拉起指定的子进程命令（如 npx 或 python）；
//   3. 子进程的 STDIN 重定向至父进程的可写管道端，STDOUT 重定向至父进程的可读管道端；
//   4. 子进程的 STDERR 保持继承，直接输出到终端控制台，便于查看服务端调试日志；
//   5. 父进程起一个后台工作线程（Reader Thread），按行读取子进程的标准输出并解析为 JSON。
//
// 跨平台考量：本实现核心适配 POSIX（macOS / Linux），并封装清晰的进程状态管理。
// ============================================================================

#include "providers/mcp/transport/ITransport.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#endif

namespace mio {
namespace mcp {

class ProcessTransport : public ITransport {
public:
    ProcessTransport(std::string command,
                     std::vector<std::string> args = {},
                     std::map<std::string, std::string> env = {});
    ~ProcessTransport() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override;

    bool send(const nlohmann::json& message) override;

    void setOnMessage(std::function<void(const nlohmann::json&)> callback) override {
        onMessage_ = std::move(callback);
    }

    void setOnError(std::function<void(const std::string&)> callback) override {
        onError_ = std::move(callback);
    }

private:
    void readerLoop();

    std::string command_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> env_;

    std::atomic<bool> running_{false};
    std::thread readerThread_;
    mutable std::mutex writeMtx_;
    std::mutex joinMtx_;  // 串行化非读线程的 join/读端关闭，避免双重 join

    std::function<void(const nlohmann::json&)> onMessage_;
    std::function<void(const std::string&)> onError_;

#ifndef _WIN32
    // 句柄用原子量：onError 回调运行在读线程上，stop() 可能被读线程与外部
    // 线程同时触发；exchange 保证 close/kill 只执行一次，也不会 join 自己。
    std::atomic<pid_t> childPid_{-1};
    std::atomic<int> stdinWriteFd_{-1};
    std::atomic<int> stdoutReadFd_{-1};
#else
    void* childProcessHandle_ = nullptr;
    void* stdinWriteHandle_ = nullptr;
    void* stdoutReadHandle_ = nullptr;
#endif
};

} // namespace mcp
} // namespace mio
