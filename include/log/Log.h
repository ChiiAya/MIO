#pragma once
// ============================================================================
// 轻量日志模块 —— 其他模块打印日志的唯一入口
//
// 职责：等级过滤 + 时间戳 + 来源标签，统一输出到 stderr。
// 替代各模块散落的 `std::cerr << "[模块名] ..."` 手工拼日志。
//
//   - 等级：Debug < Info < Warn < Error，低于当前等级的日志直接丢弃；
//   - 标签：约定传模块名（"ToolLoop" / "ChatArchive" / ...），便于过滤定位；
//   - 默认等级 Info；环境变量 MIO_LOG_LEVEL=debug|info|warn|error 可覆盖。
//
// 用法：
//   log::info("ToolLoop", "调用工具: " + name);
//   log::warn("ChatArchive", std::string("历史文件解析失败: ") + e.what());
// ============================================================================

#include <cstdint>
#include <string>
#include <string_view>

namespace mio {
namespace log {

enum class Level : std::uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// 设置全局最小输出等级（低于它的日志被丢弃）
void setLevel(Level min);
// 当前全局等级
Level level();
// 解析等级名："debug"/"info"/"warn"/"warning"/"error"（不区分大小写），失败返回 false
bool parseLevel(const std::string& name, Level& out);
// 从环境变量 MIO_LOG_LEVEL 读取等级（未设置或非法则保持原状）；程序入口调用一次
void initFromEnvironment();

// 底层入口：四个便捷函数都转到这里。线程安全（内部加锁）。
void write(Level lvl, std::string_view tag, const std::string& message);

inline void debug(std::string_view tag, const std::string& msg) {
    write(Level::Debug, tag, msg);
}
inline void info(std::string_view tag, const std::string& msg) {
    write(Level::Info, tag, msg);
}
inline void warn(std::string_view tag, const std::string& msg) {
    write(Level::Warn, tag, msg);
}
inline void error(std::string_view tag, const std::string& msg) {
    write(Level::Error, tag, msg);
}

} // namespace log
} // namespace mio
