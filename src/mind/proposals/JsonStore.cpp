#include "mind/proposals/JsonStore.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace mio {

EpochClock systemEpochClock() {
    return []() -> std::int64_t {
        return static_cast<std::int64_t>(std::time(nullptr));
    };
}

namespace jsonstore {

bool readFile(const std::filesystem::path& file, std::string* out,
              std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "无法打开文件: " + file.string();
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (in.bad()) {
        if (error) *error = "读取失败: " + file.string();
        return false;
    }
    if (out) *out = std::move(text);
    return true;
}

bool writeAtomic(const std::filesystem::path& file, const std::string& content,
                 std::string* error) {
    std::error_code ec;
    const auto parent = file.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) *error = "无法创建目录: " + parent.string();
            return false;
        }
    }
    // 临时文件必须与目标同目录（rename 跨文件系统会失败），带 pid 防多进程互踩
    const std::string tmp =
        file.string() + ".tmp-" + std::to_string(static_cast<long>(::getpid()));
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (error) *error = std::string("无法创建临时文件: ") + std::strerror(errno);
        return false;
    }
    const char* p = content.data();
    std::size_t left = content.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::string("写入失败: ") + std::strerror(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        if (error) *error = std::string("fsync 失败: ") + std::strerror(errno);
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        if (error) *error = std::string("close 失败: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), file.string().c_str()) != 0) {
        if (error) *error =
            std::string("rename 失败: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool backupIfAbsent(const std::filesystem::path& file, std::int64_t epoch,
                    std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return true;
    const std::filesystem::path bak =
        file.string() + ".bak-" + std::to_string(epoch);
    if (std::filesystem::exists(bak, ec)) return true;  // 幂等：同秒重试不重复备份
    std::filesystem::copy_file(file, bak,
                               std::filesystem::copy_options::none, ec);
    if (ec) {
        if (error) *error = "备份失败: " + bak.string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

bool parse(const std::string& text, nlohmann::json* out, std::string* error) {
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        if (out) *out = std::move(j);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
}

bool loadJsonFile(const std::filesystem::path& file, nlohmann::json* out,
                  bool* exists, std::string* error) {
    if (exists) *exists = false;
    std::string text;
    std::string readErr;
    if (!readFile(file, &text, &readErr)) {
        if (!readErr.empty()) {
            if (error) *error = readErr;
            return false;
        }
        return false;  // 不存在：正常情况（首次运行）
    }
    if (exists) *exists = true;
    if (text.empty()) {
        if (error) *error = "文件为空: " + file.string();
        return false;
    }
    return parse(text, out, error);
}

} // namespace jsonstore
} // namespace mio
