#pragma once
// ============================================================================
// JSON 持久化公共工具（子任务 C）
//
// 为什么单独抽出来：
//   * 提案 / 事实 / 认知 / 关系图谱四个落盘点必须行为一致 —— 都要 schema+version、
//     都要原子写（tmp → flush/fsync → rename），否则一次崩溃就会留下半截 JSON，
//     而"迁移失败不得破坏原文件"是硬约束；
//   * 时钟必须可注入（测试用假时钟，落盘时间一律 epoch seconds）。
//
// 注意：本文件不依赖任何契约头，避免 store 之间的循环包含。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace mio {

// 可注入时钟：epoch seconds（禁止把 monotonic 时间落盘）
using EpochClock = std::function<std::int64_t()>;

// 默认时钟：系统时间（UTC epoch seconds）
EpochClock systemEpochClock();

namespace jsonstore {

// 读取整个文件。文件不存在返回 false 且 error 为空；其他错误 error 非空。
bool readFile(const std::filesystem::path& file, std::string* out,
              std::string* error);

// 原子写：先写同目录临时文件（含 pid，避免多进程互踩），flush + fsync 后
// rename 覆盖目标。任一步失败都不触碰原文件，返回 false 并写 error。
bool writeAtomic(const std::filesystem::path& file, const std::string& content,
                 std::string* error);

// 备份 file → file.bak-<epoch>。备份已存在则视为成功（迁移可重试且幂等）。
// 源文件不存在返回 true（无需备份）。
bool backupIfAbsent(const std::filesystem::path& file, std::int64_t epoch,
                    std::string* error);

// 解析 JSON 文本；失败返回 false 并写 error（调用方不得覆盖原文件）。
bool parse(const std::string& text, nlohmann::json* out, std::string* error);

// 读取 + 解析。文件不存在返回 false 且 exists=false。
bool loadJsonFile(const std::filesystem::path& file, nlohmann::json* out,
                  bool* exists, std::string* error);

} // namespace jsonstore
} // namespace mio
