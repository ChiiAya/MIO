#pragma once
// ============================================================================
// 本地时间小工具（header-only）
//   统一 Windows / POSIX 的 localtime 差异，避免每个调用点各抄一份 #ifdef。
//   注意：这里都是"本地时间"语义 —— 日志、日界、wire 时间标签都用它，
//   和 epoch 秒的存储/比较分开。
// ============================================================================

#include <cstdint>
#include <ctime>
#include <string>

namespace mio {

// epoch 秒 → 本地 std::tm（失败时返回全零）
inline std::tm localTimeOf(std::int64_t epochSeconds) {
    const std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// epoch 秒 → 本地时间字符串（默认精确到分钟，和日志/时间标签一致）
inline std::string formatLocalTime(std::int64_t epochSeconds,
                                   const char* pattern = "%Y-%m-%d %H:%M") {
    const std::tm tm = localTimeOf(epochSeconds);
    char buf[64];
    if (std::strftime(buf, sizeof(buf), pattern, &tm) == 0) return {};
    return buf;
}

// 当天本地 00:00 的 epoch 秒（日界/维护任务筛选用）。
// tm_isdst = -1 交给 mktime 判定，跨夏令时的日期才不会差一小时。
inline std::int64_t localDayStart(std::int64_t now) {
    std::tm tm = localTimeOf(now);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&tm));
}

} // namespace mio
