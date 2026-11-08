#include "mind/MindLoop.h"

#include "context/contextBuilder/ContextBuilder.h"
#include "core/message/History.h"
#include "log/Log.h"
#include "plugin/OutboundRouter.h"
#include "providers/llm/tool/ToolLoop.h"

#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mio {

namespace {
constexpr const char* kTag = "MindLoop";
}

MindLoop::MindLoop(MindLoopDeps deps) : deps_(std::move(deps)) {
    start();
}

MindLoop::~MindLoop() {
    stop();
}

void MindLoop::start() {
    std::lock_guard<std::mutex> lock(inboxMtx_);
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread(&MindLoop::workerLoop, this);
}

void MindLoop::stop() {
    {
        std::lock_guard<std::mutex> lock(inboxMtx_);
        if (!worker_.joinable()) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MindLoop::updateServices(std::shared_ptr<Llm> llm,
                              std::shared_ptr<ContextBuilder> builder,
                              std::string systemPrompt,
                              MessageSplitterConfig splitter) {
    std::lock_guard<std::mutex> lock(inboxMtx_);
    if (llm) deps_.llm = std::move(llm);
    if (builder) deps_.builder = std::move(builder);
    if (!systemPrompt.empty()) deps_.systemPrompt = std::move(systemPrompt);
    deps_.splitter = splitter;
}

void MindLoop::setHistory(std::vector<Msg> msgs) {
    std::lock_guard<std::mutex> lock(inboxMtx_);
    context_ = std::move(msgs);
}

void MindLoop::postTurn(TurnInput input) {
    if (input.msgs.empty()) return;
    {
        std::lock_guard<std::mutex> lock(inboxMtx_);
        inbox_.push_back(std::move(input));
    }
    cv_.notify_one();
}

void MindLoop::runMaintenancePrompt(const std::string& prompt) {
    if (prompt.empty()) return;
    Msg ask{Role::User};
    ask.text = prompt;
    postTurn(TurnInput{{std::move(ask)}, "", ""});
}

MessageToolContext MindLoop::activeMessageContext() const {
    return MessageToolContext{activeSource_, activeChannelId_};
}

std::string MindLoop::activeChannelId() const {
    return activeChannelId_;
}

void MindLoop::workerLoop() {
    while (true) {
        std::vector<TurnInput> batch;
        {
            std::unique_lock<std::mutex> lock(inboxMtx_);
            cv_.wait(lock, [this] { return !inbox_.empty() || stop_; });
            if (stop_ && inbox_.empty()) break;
            batch.swap(inbox_);
        }

        for (auto& turn : batch) {
            try {
                processTurn(std::move(turn));
            } catch (const std::exception& e) {
                log::error(kTag, std::string("回合执行异常: ") + e.what());
            }
        }
    }
}

void MindLoop::processTurn(TurnInput input) {
    if (input.msgs.empty()) return;
    if (!deps_.llm || !deps_.builder) {
        throw std::runtime_error("MindLoop 服务未就绪");
    }

    generation_++;
    activeSource_ = input.source;
    activeChannelId_ = input.channelId;

    for (auto& m : input.msgs) {
        context_.push_back(std::move(m));
    }

    BuildInput in;
    in.systemPrompt = deps_.systemPrompt;
    in.context = &context_;
    auto built = deps_.builder->build(in);

    ChatRequest req = std::move(built.request);
    req.tools = deps_.registry.defs();

    if (deps_.beforeLlmHook) {
        deps_.beforeLlmHook(req.messages);
    }

    std::optional<Msg> completion;
    ChatResponse resp = runToolLoop(
        *deps_.llm, deps_.registry, req, {},
        [this, &completion](const Msg& m) {
            if (m.role == Role::Assistant && m.toolCalls.empty()) {
                completion = m;
                return;
            }
            context_.push_back(m);
        });

    if (!resp.text.empty() && deps_.emitOutbound && !activeChannelId_.empty()) {
        if (!OutboundRouter::hasDeliverableText(resp.text, deps_.splitter)) {
            log::info(kTag, "模型回复仅含空白或分段符，本轮按静默处理");
        } else {
            Outbound out;
            out.source = activeSource_;
            out.channelId = activeChannelId_;
            out.text = resp.text;
            deps_.emitOutbound(out);
        }
    }

    if (completion) {
        context_.push_back(*completion);
    }
    history::stripCompletedMedia(context_);
}

} // namespace mio
