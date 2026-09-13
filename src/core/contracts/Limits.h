#pragma once
// ============================================================================
// 统一预算常量（共享，冻结）
//
// 见文档「工具和插件接口」：
//   * 读取工具默认 20 条、最多 100 条、每次总计最多 12000 UTF-8 字节；
//   * 写入正文最多 4000 字节，query 最多 1000 字节；
//   * 超限写入返回 LIMIT_EXCEEDED；读取返回截断标记和游标。
// 这些是"可调整的实施默认值"，不是用户逐项确认的产品需求。
// ============================================================================

#include <cstddef>
#include <string>

namespace mio {
namespace limits {

inline constexpr std::size_t kReadDefaultLimit = 20;   // 读取工具默认条数
inline constexpr std::size_t kReadMaxLimit = 100;      // 读取工具单次最大条数
inline constexpr std::size_t kReadMaxBytes = 12000;    // 每次读取总计最大 UTF-8 字节
inline constexpr std::size_t kWriteMaxBytes = 4000;    // 写入正文上限
inline constexpr std::size_t kQueryMaxBytes = 1000;    // 查询串上限
inline constexpr std::size_t kSingleMessageMaxBytes = 4000;  // 单条正文截断阈值
inline constexpr std::size_t kToolResultMaxBytes = 12000;    // 工具返回上限

// UTF-8 安全字节截断：不切碎多字节序列，不产生非法 UTF-8。
// 返回截断后的字符串；truncated 置位表示发生了截断。
std::string truncateUtf8(const std::string& s, std::size_t maxBytes, bool* truncated);

// UTF-8 安全字节截断（不需要截断标记时）
inline std::string truncateUtf8(const std::string& s, std::size_t maxBytes) {
    return truncateUtf8(s, maxBytes, nullptr);
}

inline std::size_t utf8Bytes(const std::string& s) { return s.size(); }

} // namespace limits
} // namespace mio
