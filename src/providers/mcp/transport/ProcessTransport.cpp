#include "providers/mcp/transport/ProcessTransport.h"
#include "log/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <sstream>
#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

namespace mio {
namespace mcp {

ProcessTransport::ProcessTransport(std::string command,
                                   std::vector<std::string> args,
                                   std::map<std::string, std::string> env)
    : command_(std::move(command)),
      args_(std::move(args)),
      env_(std::move(env)) {}

ProcessTransport::~ProcessTransport() {
    stop();
}

bool ProcessTransport::start(){
    if(running_.load()){
        return true;
    }
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);

    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};

    if (pipe(stdinPipe) != 0) {
        if (onError_) onError_("创建子进程管道失败: " + std::string(strerror(errno)));
        return false;
    }
    if (pipe(stdoutPipe) != 0) {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        if (onError_) onError_("创建子进程管道失败: " + std::string(strerror(errno)));
        return false;
    }
    
    pid_t pid = fork();
    if(pid < 0){
        close(stdinPipe[0]); close(stdinPipe[1]);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        if (onError_) onError_("Fork 子进程失败: " + std::string(strerror(errno)));
        return false;
    }
    
    if(pid == 0){
        //子进程上下文
        close(stdinPipe[1]);
        dup2(stdinPipe[0], STDIN_FILENO);
        close(stdinPipe[0]);

        close(stdoutPipe[0]);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdoutPipe[1]);

        for (const auto& [k, v] : env_) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        std::vector<char*> c_args;
        c_args.push_back(const_cast<char*>(command_.c_str()));
        for (const auto& arg : args_) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        execvp(command_.c_str(), c_args.data());

        // 如果 execvp 返回，说明执行失败
        std::cerr << "[ProcessTransport] 无法执行命令: " << command_
                  << " (" << strerror(errno) << ")\n";
        _exit(127);
    }

    // ---- 父进程上下文 ----
    close(stdinPipe[0]);   // 关闭不需要的读端
    close(stdoutPipe[1]);  // 关闭不需要的写端

    childPid_.store(pid);
    stdinWriteFd_.store(stdinPipe[1]);
    stdoutReadFd_.store(stdoutPipe[0]);
    running_.store(true);

    // 启动独立读线程
    readerThread_ = std::thread(&ProcessTransport::readerLoop, this);
    return true;

#else
    if (onError_) onError_("Windows 平台的 ProcessTransport 暂未完全适配");
    return false;
#endif
}


void ProcessTransport::stop() {
    // 幂等：可能被外部线程（析构/管理器）与读线程（onError 回调）同时调用。
    // 注意不能在这里提前 return —— 读线程自行退出后，std::thread 仍处于
    // joinable 状态，必须由后续一次 stop() 负责 join，否则析构会 terminate。
    running_.store(false);

#ifndef _WIN32
    // 关闭 stdin 写端（让子进程读到 EOF）；exchange 保证只 close 一次
    const int writeFd = stdinWriteFd_.exchange(-1);
    if (writeFd >= 0) {
        close(writeFd);
    }

    // 结束并回收子进程；exchange 保证只 kill/wait 一次
    const pid_t pid = childPid_.exchange(-1);
    if (pid > 0) {
        kill(pid, SIGTERM);

        // 等待最多 500ms
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 10; ++i) {
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                exited = true;
                break;
            }
            if (r < 0) {  // ECHILD：已被其它路径回收
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // 超时仍未退出则强杀 (SIGKILL)
        if (!exited) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }

    // 关键：onError 回调运行在读线程上，它可能同步调用 stop()。
    // join 自己会抛 std::system_error 并导致 std::terminate，这里必须跳过；
    // 该线程退出后由下一次（外部线程的）stop() 或析构完成 join。
    const bool calledFromReader =
        readerThread_.joinable() &&
        readerThread_.get_id() == std::this_thread::get_id();

    if (!calledFromReader) {
        // 只有非读线程做回收；joinMtx_ 保证并发 stop() 不会双重 join
        std::lock_guard<std::mutex> lock(joinMtx_);
        if (readerThread_.joinable()) {
            readerThread_.join();
        }
        // 读端正常情况下由读线程自己 fclose；fdopen 失败等早退路径在这里兜底
        const int readFd = stdoutReadFd_.exchange(-1);
        if (readFd >= 0) {
            close(readFd);
        }
    }
#endif
}

bool ProcessTransport::isRunning() const {
    return running_.load();
}

bool ProcessTransport::send(const nlohmann::json& message) {
    if (!running_.load()) {
        return false;
    }

#ifndef _WIN32
    if (stdinWriteFd_ < 0) {
        return false;
    }

    // JSON-RPC over stdio 采用单行分帧，每条消息以换行符结束
    std::string payload = message.dump();
    payload.push_back('\n');

    std::string error;
    {
        std::lock_guard<std::mutex> lock(writeMtx_);
        std::size_t written = 0;
        while (written < payload.size()) {
            const ssize_t n = write(stdinWriteFd_,
                                    payload.data() + written,
                                    payload.size() - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = "写入子进程管道失败: " + std::string(strerror(errno));
                break;
            }
            written += static_cast<std::size_t>(n);
        }
    }

    if (!error.empty()) {
        if (onError_) onError_(error);
        return false;
    }
    return true;
#else
    (void)message;
    if (onError_) onError_("Windows 平台的 ProcessTransport 暂未完全适配");
    return false;
#endif
}

void ProcessTransport::readerLoop(){
#ifndef _WIN32 
    const int readFd = stdoutReadFd_.load();
    if (readFd < 0) return;

    FILE*fp = fdopen(readFd, "r");
    if(!fp) {
        // 先标记失效再回调：上层 stop() 会走"读线程自停止"分支，不 join 自己
        running_.store(false);
        if(onError_) onError_("fdopen包装读管道失败");
        return;
    }

    char* lineBuf = nullptr;
    size_t lineCap = 0;
    ssize_t nread = 0;

    while (running_.load()) {
        nread = getline(&lineBuf, &lineCap, fp);
        if (nread == -1) {
            // EOF 或发生读错误（子进程已关闭标准输出）
            break;
        }

        if (nread <= 0) continue;

        std::string lineStr(lineBuf, static_cast<std::size_t>(nread));
        // 去除换行符与多余空白
        while (!lineStr.empty() && (lineStr.back() == '\n' || lineStr.back() == '\r' || lineStr.back() == ' ')) {
            lineStr.pop_back();
        }
        if (lineStr.empty()) continue;

        // 解析单行 JSON-RPC 消息
        try {
            auto j = nlohmann::json::parse(lineStr);
            if (onMessage_) {
                onMessage_(j);
            }
        } catch (const std::exception& e) {
            // 忽略非 JSON 行（部分底层工具可能偶尔向 stdout 打少量非标准文本）
            log::warn("ProcessTransport", "解析子进程输出 JSON 失败: " + std::string(e.what())
                      + " 内容: " + lineStr);
        }
    }

    if(lineBuf){
        free(lineBuf);
    }
    fclose(fp);
    stdoutReadFd_.store(-1);

    // 同上：先置位再回调，避免回调链 stop() -> join(自身)
    const bool unexpected = running_.exchange(false);
    if(unexpected){
        if(onError_) onError_("子进程标准输出管道已关闭（子进程退出）");
    }
#endif
}

}
}
