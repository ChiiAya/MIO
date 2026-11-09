#include "mind/MindLoop.h"

#include "context/contextBuilder/ContextBuilder.h"
#include "context/costEstimator/Tokens.h"
#include "core/message/History.h"
#include "log/Log.h"
#include "plugin/OutboundRouter.h"
#include "providers/llm/tool/ToolLoop.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mio {

namespace {
constexpr const char* kTag = "MindLoop";

// 工具集指纹：工具定义是请求前缀的一部分，插件热重载/启用状态变化会让上游缓存整体失效
std::string toolSetSignature(const std::vector<ToolDef>& defs) {
    std::string sig;
    for (const auto& d : defs) {
        sig += d.name;
        sig += '\x1f';
        sig += d.description;
        sig += '\x1f';
        sig += d.parametersJsonSchema.dump();
        sig += '\x1f';
        sig += d.isTerminal ? '1' : '0';
        sig += '\n';
    }
    return sig;
}
} // namespace

// 每回合在锁内拷贝出的不可变服务快照（此后 Worker 不再回读可变 deps_ 字段）
struct MindLoop::TurnServices {
    std::shared_ptr<Llm> llm;
    std::shared_ptr<ContextBuilder> builder;
    std::string systemPrompt;
    MessageSplitterConfig splitter;
    bool autoEmitTextReply = false;
    int historyKeep = 8;
};

MindLoop::MindLoop(MindLoopDeps deps) : deps_(std::move(deps)) {
    // 初始历史必须在 worker 启动前移入 context_，此后 context_ 由 worker 独占
    context_ = std::move(deps_.initialHistory);
    deps_.initialHistory.clear();
    worker_ = std::thread(&MindLoop::workerLoop, this);
}

MindLoop::~MindLoop() {
    stop();
}

void MindLoop::stop() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void MindLoop::notifyWake() {
    generation_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        wakePending_ = true;
    }
    cv_.notify_one();
}

void MindLoop::updateServices(std::shared_ptr<Llm> llm,
                              std::shared_ptr<ContextBuilder> builder,
                              std::string systemPrompt,
                              MessageSplitterConfig splitter,
                              int debounceMs,
                              bool autoEmitTextReply,
                              int historyKeep) {
    bool promptChanged = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (llm) deps_.llm = std::move(llm);
        if (builder) deps_.builder = std::move(builder);
        // 只有 systemPrompt 字节真正变化才会破坏缓存前缀；未变化时无需压缩历史
        if (!systemPrompt.empty() && systemPrompt != deps_.systemPrompt) {
            deps_.systemPrompt = std::move(systemPrompt);
            promptChanged = true;
        }
        deps_.splitter = splitter;
        deps_.debounceMs = debounceMs;
        deps_.autoEmitTextReply = autoEmitTextReply;
        deps_.historyKeep = historyKeep;
        if (promptChanged && !compactPending_) {
            compactPending_ = true;
            compactReason_ = "systemPrompt 变更";
        }
    }
    if (promptChanged) cv_.notify_one();
}

void MindLoop::requestCompact(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!compactPending_) {
            compactPending_ = true;
            compactReason_ = reason;
        }
    }
    cv_.notify_one();
}

void MindLoop::markMessageSentInTurn() {
    sentInTurn_.store(true, std::memory_order_relaxed);
}

void MindLoop::markSilentInTurn() {
    silentInTurn_.store(true, std::memory_order_relaxed);
}

MessageToolContext MindLoop::activeMessageContext() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return MessageToolContext{activeSource_, activeChannelId_};
}

std::string MindLoop::activeChannelId() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return activeChannelId_;
}

void MindLoop::workerLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] {
            return wakePending_ || compactPending_ ||
                   stop_.load(std::memory_order_relaxed);
        });
        if (stop_.load(std::memory_order_relaxed)) return;

        // 前缀缓存已失效（systemPrompt / 模型端点变化）：Worker 独占压缩历史，
        // 让紧随其后的首次请求只重算少量近端原文，而不是整段旧历史。
        if (compactPending_) {
            compactPending_ = false;
            const int keep = deps_.historyKeep;
            std::string reason = std::move(compactReason_);
            compactReason_.clear();
            lock.unlock();
            compactHistory(keep, reason);
            lock.lock();
            if (!wakePending_) continue; // 纯压缩请求：处理完继续休眠，不产生回合
        }

        // 防抖：窗口内不断有新事件到达则顺延静默计时，总等待不超过 debounce*3
        const int debounce = deps_.debounceMs;
        if (debounce > 0) {
            std::uint64_t lastGen = generation_.load(std::memory_order_relaxed);
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(debounce * 3);
            while (true) {
                const auto now = std::chrono::steady_clock::now();
                const auto until = std::min(deadline, now + std::chrono::milliseconds(debounce));
                cv_.wait_until(lock, until, [this, lastGen] {
                    return stop_.load(std::memory_order_relaxed) ||
                           generation_.load(std::memory_order_relaxed) != lastGen;
                });
                if (stop_.load(std::memory_order_relaxed)) return;
                if (generation_.load(std::memory_order_relaxed) != lastGen) {
                    lastGen = generation_.load(std::memory_order_relaxed);
                    continue; // 静默窗口顺延
                }
                break; // 静默达到 debounce
            }
        }

        wakePending_ = false;

        TurnServices snap;
        snap.llm = deps_.llm;
        snap.builder = deps_.builder;
        snap.systemPrompt = deps_.systemPrompt;
        snap.splitter = deps_.splitter;
        snap.autoEmitTextReply = deps_.autoEmitTextReply;
        snap.historyKeep = deps_.historyKeep;
        lock.unlock();

        // 先 drain 再取 turnGen：本批次刚取走的事件不得把本轮误判为「被新事件打断」
        GateBatch batch = deps_.gate.drainBatch(true);
        const std::uint64_t turnGen = generation_.load(std::memory_order_relaxed);

        if (batch.msgs.empty()) continue;

        try {
            processTurn(batch, snap, turnGen);
        } catch (const std::exception& e) {
            log::error(kTag, std::string("回合执行异常: ") + e.what());
        }
    }
}

void MindLoop::processTurn(GateBatch& batch, const TurnServices& snap, std::uint64_t turnGen) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        activeSource_ = batch.source;
        activeChannelId_ = batch.channelId;
        sentInTurn_.store(false, std::memory_order_relaxed);
        silentInTurn_.store(false, std::memory_order_relaxed);
    }

    // 工具定义同样是请求前缀：插件热重载等导致工具集变化时，缓存前缀一并失效
    std::vector<ToolDef> tools = deps_.registry.defs();
    const std::string toolSig = toolSetSignature(tools);
    if (!lastToolSig_.empty() && toolSig != lastToolSig_)
        compactHistory(snap.historyKeep, "工具集变更");
    lastToolSig_ = toolSig;

    for (auto& m : batch.msgs) {
        context_.push_back(std::move(m));
    }

    if (!snap.llm || !snap.builder) {
        throw std::runtime_error("MindLoop 服务未就绪");
    }

    BuildInput in;
    in.systemPrompt = snap.systemPrompt;
    in.context = &context_;
    auto built = snap.builder->build(in);

    ChatRequest req = std::move(built.request);
    req.tools = std::move(tools);

    // 上下文超水位截断后重冷启：由 Runtime 重建 systemPrompt 并返回新 prompt
    if (built.reColdStarted && deps_.onReColdStart) {
        req.systemPrompt = deps_.onReColdStart();
    }

    if (deps_.beforeLlmHook) {
        deps_.beforeLlmHook(req.messages);
    }

    std::optional<Msg> completion;
    ToolLoopOptions loopOpt;
    loopOpt.shouldInterrupt = [this, turnGen] {
        return stop_.load(std::memory_order_relaxed) ||
               generation_.load(std::memory_order_relaxed) != turnGen;
    };

    ChatResponse resp = runToolLoop(
        *snap.llm, deps_.registry, req, loopOpt,
        [this, &completion](const Msg& m) {
            if (m.role == Role::Assistant && m.toolCalls.empty()) {
                completion = m;
                return;
            }
            context_.push_back(m);
        });

    // 被新事件打断：未完成的回复不外发；新事件已由 notifyWake 置位，下一轮会处理
    if (resp.finishReason == "interrupted" && resp.text.empty()) {
        history::stripCompletedMedia(context_);
        return;
    }

    // 自动外发（默认关闭）：本轮已通过 send_message 主动发送、或模型已标记静默时不再重复
    if (!resp.text.empty() && silentInTurn_.load(std::memory_order_relaxed))
        log::info(kTag, "本轮已标记静默，跳过最终文本自动外发");
    if (!resp.text.empty() && snap.autoEmitTextReply &&
        !sentInTurn_.load(std::memory_order_relaxed) &&
        !silentInTurn_.load(std::memory_order_relaxed) && deps_.emitOutbound) {
        std::string source;
        std::string channelId;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            source = activeSource_;
            channelId = activeChannelId_;
        }
        if (!channelId.empty() && OutboundRouter::hasDeliverableText(resp.text, snap.splitter)) {
            Outbound out;
            out.source = source;
            out.channelId = channelId;
            out.text = resp.text;
            if (!deps_.emitOutbound(out)) {
                log::warn(kTag, "自动外发失败");
            }
        } else {
            log::info(kTag, "本轮无自动外发目标或文本不可投递，按静默处理");
        }
    }

    if (completion) {
        context_.push_back(*completion);
    }
    history::stripCompletedMedia(context_);
}

// 前缀缓存失效后的历史压缩：仅保留近 keep 条原文（工具调用边界由 safeTailStart 保证安全）
void MindLoop::compactHistory(int keep, const std::string& reason) {
    if (context_.empty()) return;
    const std::size_t before = context_.size();
    const std::int64_t tokensBefore = estimateTokens(context_);
    const std::size_t start = history::safeTailStart(context_, keep);
    if (start == 0) return; // 本来就不超 keep 条，或正处于不可截断的工具调用中途
    context_.erase(context_.begin(), context_.begin() + static_cast<std::ptrdiff_t>(start));
    log::info(kTag, "前缀缓存失效，历史已压缩: reason=" + reason + " " +
                        std::to_string(before) + "条/" + std::to_string(tokensBefore) +
                        "tok -> " + std::to_string(context_.size()) + "条/" +
                        std::to_string(estimateTokens(context_)) + "tok");
}

} // namespace mio
