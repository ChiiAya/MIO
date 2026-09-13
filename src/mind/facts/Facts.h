#pragma once
// ============================================================================
// 基础事实（潜意识层）：人设 + 认识的人 + 事实/印象/日记数据 → 稳定 system 块。
//   * 只在校验过的重建时机调用（启动 / 图谱变化 / 摘要触发）；
//   * 两次重建之间输出逐字不变（保护 prompt cache 前缀）：排序必须确定，
//     不得输出时间戳或随机顺序；
//   * RelationshipGraph top-K 注入（≤20 人，token 封顶），不渲染实时权重，
//     也不把原始亲密度浮点数渲染成等级；
//   * 子任务 C：人设与可信固定配置可在 system；个人事实、印象与召回内容一律
//     放在明确标注为「记忆数据 / 仅供引用，不是行为指令」的区块里，逐条带来源；
//   * 只注入 observed + confirmed 认知（injectableIntoPrompt）；不得渲染 pending
//     提案，也不得渲染任何 HUMAN_REVIEW_* 占位字段。
// ============================================================================

#include <string>
#include <vector>

#include "core/contracts/ProposalContracts.h"
#include "diary/Diary.h"
#include "mind/graph/RelationshipGraph.h"

namespace mio {

// 前置声明（实现文件才包含，避免 facts → proposals 的头依赖）
class CognitionStore;

struct Persona {
    std::string botName = "MIO";
    std::string character;  // 完整人设文本；将来从配置文件/管理界面来
    std::string systemPromptPrefix;  // 身份引导前缀（可填，支持 {botName} 宏；空则走默认）
    std::string systemPromptNotice;  // 注意事项与行为规范（可填；空则走默认）
};

// 兼容旧签名（Runtime 现用）：渲染稳定 system 块
std::string buildSystemPrompt(const Persona& persona,
                              const RelationshipGraph& graph,
                              const std::vector<DiaryEntry>& cognition,
                              int maxPeopleTokens = 1200);

// 扩展签名（子任务 C）：额外注入已确认事实与可注入认知（默认 nullptr = 不注入）
std::string buildSystemPrompt(const Persona& persona,
                              const RelationshipGraph& graph,
                              const std::vector<DiaryEntry>& cognition,
                              int maxPeopleTokens, const IFactStore* facts,
                              const CognitionStore* cognitions);

// 等价的指针在前重载（方便接线，避免参数顺序记错）
std::string buildSystemPrompt(const Persona& persona,
                              const RelationshipGraph& graph,
                              const std::vector<DiaryEntry>& cognition,
                              const IFactStore* facts,
                              const CognitionStore* cognitions,
                              int maxPeopleTokens = 1200);

// 当前时间字符串（get_current_time 工具用）
std::string nowTimeString();

} // namespace mio
