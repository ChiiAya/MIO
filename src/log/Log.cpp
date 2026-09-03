#include "log/Log.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <utility>

namespace mio {
namespace log {

namespace {

Level g_minLevel = Level::Info;
std::mutex g_mutex;  // 保护 stderr 输出，避免多线程日志交错

const char* levelName(Level lvl) {
    switch (lvl) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?";
}

} // namespace

void setLevel(Level min) { g_minLevel = min; }

Level level() { return g_minLevel; }

bool parseLevel(const std::string& name, Level& out) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));

    if (lower == "debug") out = Level::Debug;
    else if (lower == "info") out = Level::Info;
    else if (lower == "warn" || lower == "warning") out = Level::Warn;
    else if (lower == "error") out = Level::Error;
    else return false;
    return true;
}

void initFromEnvironment() {
    if (const char* env = std::getenv("MIO_LOG_LEVEL")) {
        Level parsed;
        if (parseLevel(env, parsed)) setLevel(parsed);
    }
}

void write(Level lvl, std::string_view tag, const std::string& message) {
    if (lvl < g_minLevel) return;  // 等级不够：直接丢弃

    // 本地时间戳，只精确到秒（日志是用来排障的，不是计时器）
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d",  //
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "%s [%s] [%.*s] %s\n", stamp, levelName(lvl),
                 static_cast<int>(tag.size()), tag.data(), message.c_str());
}

} // namespace log
} // namespace mio
