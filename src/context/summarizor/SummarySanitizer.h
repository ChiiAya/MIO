#pragma once
// ============================================================================
// SummarySanitizer —— 摘要正文的最后一道闸门（子任务 B，独占目录）
//
// 硬约束（docs/operations/memory-system-refactor.md「子任务 B / 总体不变量」）：
//   * 摘要正文不得包含 reasoning、系统提示、API key、内部工具实现细节
//     或人工审核占位字段；
//   * 命中时【不得写入】：能安全删除就删除并记录，否则令 ok=false 并给出 error；
//   * reasoning 永远不进入经历摘要输入，也不提供 include_reasoning 开关。
//
// 为什么按【行】清洗而不是整段拒绝：
//   模型偶发把提示词片段、字段名或凭据回显到正文时，丢掉污染行远好于丢掉整段
//   经历；只有当全部内容行都被判定为污染时才失败 —— 此时若静默写空摘要，
//   等于把失败伪装成成功，反而会污染长期记忆。
//
// 本文件只做"文本净化"这一件可单测的纯函数，不依赖 LLM/SQLite/日志。
// ============================================================================

#include <string>
#include <vector>

namespace mio {

struct SanitizeResult {
    bool ok = true;         // false = 清洗后没有可写入的内容（调用方必须视为失败）
    bool modified = false;  // 是否发生删除/剔除（供审计与测试）
    std::string text;       // 清洗后的正文（ok=false 时为空）
    std::string error;      // ok=false 时的原因（可写进 SummaryOutcome::error）
    // 命中的规则名（审计/测试用；绝不含被删除的原文，避免二次泄漏）
    std::vector<std::string> hits;
};

// extraForbidden：调用方注册的"系统提示片段"（摘要器 prompt 的特征句）。
// 命中即视为提示词回显，整行删除。
SanitizeResult sanitizeSummaryText(
    const std::string& text,
    const std::vector<std::string>& extraForbidden = {});

// UTF-8 合法性校验：拒绝 overlong、代理区（U+ D800~DFFF）与 > U+10FFFF。
// 用途：验证标记行解析没有把多字节字符切碎 —— 历史缺陷
// `line.find_first_of("：:")` 会把全角冒号的首字节当成匹配位，导致 substr
// 从续字节开始（topic 以 0xBC 开头、非法 UTF-8）。
bool isValidUtf8(const std::string& s);

// 按 UTF-8 字符边界截断（不产生半个字符）。摘要正文/提案字段落库前统一用它。
std::string cutUtf8Safe(const std::string& s, std::size_t maxBytes);

} // namespace mio
