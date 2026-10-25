#pragma once
// ============================================================================
// Memory Provider 抽象接口（预留）
//
// 预留给外部记忆提供商（如 Mem0 / Chroma / Qdrant / 图数据库）的抽象接口。
// 现阶段 MIO 核心默认基于 SQLite + BLOB 向量检索，未来可通过本接口实现插件化接入。
// ============================================================================

#include <string>
#include <vector>

namespace mio {

class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;
};

} // namespace mio

