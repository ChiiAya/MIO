#pragma once
// ============================================================================
// MindTools —— 单意识流核心内置工具集：
//   - get_current_time
//   - keepsilent / keep_silent / stay_silent（非终结：标记本轮不回复，思考继续）
//   - end_turn（终结：停止思考，结束本回合且不发消息）
//   - wait / sleep
//   - query_events
//   - set_attention_rule / list_attention_rules
// ============================================================================

#include <cstdint>
#include <functional>

namespace mio {

class Achieve;
class AttentionGate;
class ToolRegistry;

struct MindToolsDeps {
    Achieve& achieve;
    AttentionGate& gate;
    std::function<std::uint64_t()> currentGeneration;
    std::function<void()> markSilent; // keepsilent：标记本轮静默（抑制最终文本自动外发）
};

void registerMindTools(ToolRegistry& registry, MindToolsDeps deps);

} // namespace mio

