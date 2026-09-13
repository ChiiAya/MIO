#pragma once
// ============================================================================
// SummaryManager —— 经历摘要 / 上下文压缩 / 模型主动保存的生成端（子任务 B）
//
// 公开契约（冻结，F/D/主 agent 依赖，签名不得改变）：
//   * SummaryOutcome summarize(const SummaryRequest&);
//   * SummaryOutcome summarizeWithVerdict(const std::vector<Msg>&) const;
//   * 私有 SummaryOutcome generate(const std::vector<Msg>&) const;
//
// 硬约束（docs/operations/memory-system-refactor.md）：
//   * reasoning 一律排除：材料用【副本】构造并清空 reasoningContent；
//   * 系统提示、API key、内部工具实现细节绝不进入摘要材料（工具消息只留工具名）；
//   * 按 kind 区分行为：EpisodicMemory 产出经历摘要 + 私密性 + 话题 + 事实提案 +
//     关系变化提案；ContextCompaction 只产出压缩摘要（不产出提案、不提升可见性）；
//     Manual 从简；
//   * 提案一律 pending / proposed / HUMAN_REVIEW_PENDING，actor 空、reviewedAt 0、
//     approvedBy 空；模型只能提供 predicate/object/confidence，其余服务端填；
//   * 摘要正文经 SummarySanitizer 兜底校验，命中禁止内容不得写入；
//   * 模型调用异常必须捕获并转 ok=false，绝不让异常穿透到调度线程；
//   * onSummary_ sink 只在公开入口发一次（generate 内不发）。
// ============================================================================

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "providers/llm/Llm.h"
#include "core/contracts/SummaryContracts.h"
#include "core/message/Message.h"

namespace mio {

class SummaryManager {
public:
    SummaryManager(int maxSummaryChars, std::shared_ptr<Llm> llm);

    // 摘要产出通知（可选，组合根注入）：每次真正产出摘要后回调一次，
    // 携带结果与私密性判定 —— 记忆系统的写入流程以此咽喉点为唯一入口。
    using SummarySink = std::function<void(const SummaryOutcome&)>;
    void setOnSummary(SummarySink sink) { onSummary_ = std::move(sink); }

    // 契约入口（B 实现）：按 SummaryRequest 生成摘要/经历摘要/压缩摘要。
    // material 必须已剔除 reasoning；失败返回 ok=false 而不是抛出。
    // 注意：即便调用方忘了过滤 reasoning，本实现也会用副本再过滤一次。
    SummaryOutcome summarize(const SummaryRequest& request);

    // 兼容入口（v6 每 N 轮摘要）：等价于 kind = context_compaction。
    // 保留以兼容既有调用方，新代码请使用 summarize()。
    SummaryOutcome summarizeWithVerdict(const std::vector<Msg>& material) const;

    // 交互式摘要的角色标签（供测试断言材料形状，不参与逻辑）
    static const char* kindLabel(SummaryKind kind);

private:
    // 实际生成（不发通知）：供 summarize / summarizeWithVerdict 共用
    SummaryOutcome generate(const std::vector<Msg>& material) const;

    // 按 kind 生成（内部实现；generate 委托到它）。不抛异常，失败置 error。
    SummaryOutcome generateFor(const SummaryRequest& request) const;

    // 摘要材料渲染：跳过 system、剥离 reasoning、工具回合只留工具名/结果不落正文
    std::string renderMaterial(const std::vector<Msg>& material) const;

    // 按 kind 的 system prompt；同时把特征句写进 sentinels 供清洗器识别回显
    std::string systemPromptFor(SummaryKind kind,
                                std::vector<std::string>* sentinels) const;

    int maxSummaryChars_;
    std::shared_ptr<Llm> llm_;
    SummarySink onSummary_;
};

} // namespace mio
