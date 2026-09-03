#pragma once
// ============================================================================
// 基础事实（潜意识层）：人设 + 认识的人 + 日记认知 → 稳定 system 块。
//   * 只在校验过的重建时机调用（启动 / 图谱变化 / 摘要触发）；
//   * 两次重建之间输出逐字不变（保护 prompt cache 前缀）；
//   * RelationshipGraph top-K 注入（≤20 人，token 封顶），不渲染实时权重。
// ============================================================================

#include <string>
#include <vector>

#include "diary/Diary.h"
#include "mind/graph/RelationshipGraph.h"

namespace mio {

struct Persona {
    std::string botName = "MIO";
    std::string character;  // 完整人设文本；将来从配置文件/管理界面来
};

// 渲染稳定 system 块：人设 + 认识的人(top-K, ≤maxPeopleTokens) + 日记认知
std::string buildSystemPrompt(const Persona& persona,
                              const RelationshipGraph& graph,
                              const std::vector<DiaryEntry>& cognition,
                              int maxPeopleTokens = 1200);

// 当前时间字符串（get_current_time 工具用）
std::string nowTimeString();

} // namespace mio
