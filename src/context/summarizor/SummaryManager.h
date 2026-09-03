#pragma once
// ============================================================================
//   * summarizeWithVerdict —— v6 每 N 轮摘要：输出摘要 + 话题 + 私密性判定
//     （话题随摘要同步更新 unit.topic；私密性供 privateRate 更新，
//     cold start 默认私密，摘要时 LLM 判定）。
// 摘要只面向上下文构建器；不覆盖原文（原文留在会话档案）。
// ============================================================================

#include <functional>
#include <string>
#include <vector>

#include "llm/Llm.h"
#include "core/message/Message.h"

namespace mio {

struct SummaryOutcome {
    std::string text;            // 累计摘要（含旧摘要；标记行已剥离）
    bool privateVerdict = false; // 摘要内容是否私密（LLM 判定，解析失败保持默认）
    std::string topic;           // 摘要器同步产出的话题（解析失败/未输出为空）
};

class SummaryManager {
public:
    SummaryManager(int maxSummaryChars, Llm& llm);

    // 摘要产出通知（可选，组合根注入）：每次真正产出摘要后回调一次，
    // 携带结果与私密性判定 —— 记忆系统的写入流程以此咽喉点为唯一入口，
    // 冷读取 / 融合重定向 / 重新冷启动三条摘要路径全覆盖，无需各自接线。
    using SummarySink = std::function<void(const SummaryOutcome&)>;
    void setOnSummary(SummarySink sink) { onSummary_ = std::move(sink); }

    // v6：摘要 + 私密性判定；输入应包含旧摘要 + 待总结的原始回合
    SummaryOutcome summarizeWithVerdict(const std::vector<Msg>& material) const;

private:
    int maxSummaryChars_;
    Llm& llm_;
    SummarySink onSummary_;
};

} // namespace mio
