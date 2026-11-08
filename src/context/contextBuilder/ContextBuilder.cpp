#include "context/contextBuilder/ContextBuilder.h"

#include <algorithm>
#include <stdexcept>

#include "context/costEstimator/Tokens.h"
#include "core/message/History.h"
#include "log/Log.h"

namespace mio {

ContextBuilder::ContextBuilder(ContextBuilderConfig cfg) : cfg_(cfg) {}

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

    if (overBudget && !msgs.empty()) {
        const std::size_t tailStart = history::safeTailStart(msgs, cfg_.rawKeep);
        if (tailStart > 0) {
            std::vector<Msg> truncated(msgs.begin() + static_cast<std::ptrdiff_t>(tailStart), msgs.end());
            ctx = truncated;
            msgs = std::move(truncated);
            r.reColdStarted = true;
            log::info("ContextBuilder", "上下文超水位，已按 rawKeep 截断旧历史");
        }
    }

    ChatRequest req;
    req.systemPrompt = in.systemPrompt;
    req.messages = std::move(msgs);
    r.request = std::move(req);
    return r;
}

} // namespace mio
