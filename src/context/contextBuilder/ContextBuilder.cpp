#include "context/contextBuilder/ContextBuilder.h"

#include <algorithm>
#include <stdexcept>

#include "context/costEstimator/Tokens.h"
#include "core/message/History.h"
#include "log/Log.h"

namespace mio {

ContextBuilder::ContextBuilder(ContextBuilderConfig cfg,
                               std::shared_ptr<SummaryManager> summary)
    : cfg_(cfg), summary_(std::move(summary)) {}

BuildResult ContextBuilder::build(const BuildInput& in) {
    if (!in.context) throw std::runtime_error("ContextBuilder: context 为空");
    std::vector<Msg>& ctx = *in.context;

    std::vector<Msg> msgs = history::repairToolHistory(ctx);
    history::stripCompletedMedia(msgs);
    std::int64_t estimated =
        estimateTokens(msgs) + estimateTokens(in.systemPrompt);

    BuildResult r;
    const bool overBudget =
        estimated > static_cast<std::int64_t>(cfg_.budgetTokens * cfg_.watermark);

    if (overBudget && !msgs.empty() && summary_) {
        const std::size_t tailStart = history::safeTailStart(msgs, cfg_.rawKeep);
        if (tailStart > 0) {
            std::vector<Msg> toSummarize(msgs.begin(),
                                         msgs.begin() + static_cast<std::ptrdiff_t>(tailStart));
            try {
                SummaryOutcome outcome = summary_->summarizeWithVerdict(toSummarize);
                if (!outcome.text.empty()) {
                    std::vector<Msg> compacted;
                    Msg summaryMsg{Role::User};
                    summaryMsg.text = "[意识流历史摘要] " + outcome.text;
                    compacted.push_back(std::move(summaryMsg));
                    compacted.insert(compacted.end(),
                                     msgs.begin() + static_cast<std::ptrdiff_t>(tailStart),
                                     msgs.end());
                    ctx = compacted;
                    msgs = std::move(compacted);
                    r.reColdStarted = true;
                    log::info("ContextBuilder", "上下文超水位，已完成意识流摘要压缩");
                }
            } catch (const std::exception& e) {
                log::warn("ContextBuilder", std::string("上下文压缩失败，保留原上下文: ") + e.what());
            }
        }
    }

    ChatRequest req;
    req.systemPrompt = in.systemPrompt;
    req.messages = std::move(msgs);
    r.request = std::move(req);
    return r;
}

} // namespace mio
