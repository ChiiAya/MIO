#pragma once

#include <functional>

namespace mio {
class ToolRegistry;
class RelationshipGraph;

// onChanged 在图谱持久化成功并释放图谱锁之后调用。
void registerContactTools(ToolRegistry& registry, RelationshipGraph& graph,
                          std::function<void()> onChanged = {});
} // namespace mio
