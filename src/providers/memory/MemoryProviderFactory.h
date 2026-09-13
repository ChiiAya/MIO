#pragma once
// ============================================================================
// MemoryProviderFactory —— 记忆后端选择（子任务 E，供主 agent 的 Runtime 接线）
//
// backend（大小写与首尾空白不敏感）：
//   * ""（空 = 未配置，取默认）/ "sqlite-local" → LocalMemoryProvider（默认后端）
//   * "unavailable"                              → UnavailableMemoryProvider
//   * "hindsight"                                → HindsightMemoryProvider（占位，恒不可用）
//   * 其它任何名称                                → UnavailableMemoryProvider（降级，不抛异常）
//
// 未知名称降级而不是抛异常 / 回落到本地库，是刻意的：配置写错时宁可"没有长期
// 召回"，也不能悄悄把数据写进一个与配置期望不符的后端。
//
// 【接口约束不等于进程隔离】见 MemoryProvider.h / LocalMemoryProvider.h。
// ============================================================================

#include "memory/manager/MemoryManager.h"
#include "providers/memory/MemoryProvider.h"

#include <memory>
#include <string>

namespace mio {

// memory 必须比返回的 provider 活得久（Runtime 组合根保证生命周期）。
// 永不返回 nullptr，也永不抛异常。
std::shared_ptr<MemoryProvider> makeMemoryProvider(const std::string& backend,
                                                   MemoryManager& memory,
                                                   const MemoryConfig& config);

} // namespace mio
