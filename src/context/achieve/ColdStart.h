#pragma once
// ============================================================================
// 冷启动（ColdStart）—— 无 LLM 的历史原文恢复
//
// 硬约束（docs/operations/memory-system-refactor.md「冷启动、上下文压缩与
// reasoning」）：
//   * 冷启动 MUST NOT 同步调用摘要模型，也 MUST NOT 写入长期记忆；
//   * 在双重预算（rawTokenBudget + maxMessages）内按时间正序恢复；
//   * 只恢复用户/助手可见正文：reasoning 一律排除，且不提供 include_reasoning
//     开关；工具调用回合整体排除（历史工具调用不得作为未完成调用重放，
//     避免留下孤立 tool 消息）；
//   * 超长单条正文按 UTF-8 安全截断并标记（Msg.truncated）；
//   * 当前请求及输出预留预算优先：调用方先用 systemPrompt + 当前回合占用
//     预算，再把剩余额度作为 rawTokenBudget 传进来；
//   * rawTokenBudget == 0 或 maxMessages == 0 表示【不恢复历史原文】。
//
// 本文件是纯逻辑（无 IO、无模型）：选择算法放在 selectColdStart，便于单测；
// Achieve 负责取档案快照，ContextBuilder / FusionRouter 负责给预算。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include "core/contracts/ArchiveRecord.h"
#include "core/message/Message.h"

namespace mio {

// 框架提醒（例如 ContextBuilder 注入的 "[框架]本会话还没有话题记录…"）：
// 不是用户/助手可见正文，不进入冷启动、摘要材料，也不计入待总结范围。
inline bool isFrameworkReminder(const std::string& text) {
    return text.rfind("[框架]", 0) == 0;
}

struct ColdStartOptions {
    int rawTokenBudget = 2000;  // 历史原文 token 预算（0 = 不恢复）
    int maxMessages = 20;       // 恢复条数上限（0 = 不恢复）
    // 已在当前上下文中的消息 ID（当前批次）：不得重复恢复
    std::vector<std::int64_t> excludeMessageIds;
};

struct ColdStartResult {
    std::vector<Msg> msgs;      // 时间正序；reasoning / 工具回合 / reasoning 标记已剔除
    bool anyTruncated = false;  // 至少一条正文被 UTF-8 安全截断（Msg.truncated = true）
    int droppedByBudget = 0;    // 因双重预算未恢复的可见消息数（可观察，不静默）
    int droppedToolRounds = 0;  // 因工具回合完整性被整体排除的消息数
    int droppedNotVisible = 0;  // 非用户/助手可见正文（框架提醒、空正文等）数
    std::int64_t fromMessageId = 0;  // 实际恢复的首条消息 ID（0 = 未恢复）
    std::int64_t toMessageId = 0;    // 实际恢复的末条消息 ID（0 = 未恢复）
};

// 从【时间正序】的档案投影中选择冷启动原文。
// 纯函数：不调用模型、不写存储；输入记录已被投影剔除 reasoning 内容
// （hasReasoning 仅表示"存在但被排除"）。
ColdStartResult selectColdStart(const std::vector<ArchiveRecord>& recordsAscending,
                                const ColdStartOptions& opts);

// 冷启动数据源：ContextBuilder 的可空依赖（Achieve 实现）。
// 用窄接口而不是直接依赖 Achieve，避免 contextBuilder ↔ achieve 双向耦合。
class IColdStartSource {
public:
    virtual ~IColdStartSource() = default;
    virtual ColdStartResult coldStart(const std::string& convKey,
                                      const ColdStartOptions& opts) = 0;
};

} // namespace mio
