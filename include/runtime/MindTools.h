#pragma once
// ============================================================================
// MindTools —— 单意识流核心内置工具集：
//   - get_current_time
//   - keepsilent / keep_silent / stay_silent
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
    std::function<void()> markKeepSilent;
    std::function<std::uint64_t()> currentGeneration;
};

void registerMindTools(ToolRegistry& registry, MindToolsDeps deps);

} // namespace mio

