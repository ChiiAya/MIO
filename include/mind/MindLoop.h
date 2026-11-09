#pragma once
// ============================================================================
// MindLoop —— 单意识流工作循环引擎（拉取式并发模型）
//
// 职责：
//   1. 维护全局唯一意识流历史 `context_`（Worker 线程独占，单向流转零锁）；
//   2. 由 notifyWake() 唤醒：generation 原子递增 + 防抖窗口收敛突发事件，
//      随后主动从 AttentionGate 拉取已就绪批次 drainBatch()；
//   3. 用栈上不可变服务快照驱动 `ContextBuilder` + `ToolLoop`，通过 emitOutbound 完成出站；
//   4. generation 作为回合代际：新事件到达即协作式中断当前工具循环，未完成回复不外发；
//   5. 维护当前活跃回合的来源与目标频道（加锁读，供消息工具查询上下文）；
//   6. 前缀缓存失效时压缩历史：systemPrompt / 工具集 / 模型端点变化都会让上游缓存前缀
//      整体失效，此时把 `context_` 压缩到近 historyKeep 条，避免旧历史按未命中缓存价格重算。
// ============================================================================

#include "config/AppConfig.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "core/event/AttentionGate.h"
#include "core/message/Message.h"
#include "providers/llm/Llm.h"
#include "providers/llm/tool/ToolRegistry.h"
#include "runtime/MessageTools.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mio {

class MindLoop {
public:
    struct MindLoopDeps {
        AttentionGate& gate;
        ToolRegistry& registry;
        std::shared_ptr<Llm> llm;
        std::shared_ptr<ContextBuilder> builder;
        std::string systemPrompt;
        MessageSplitterConfig splitter;
        std::vector<Msg> initialHistory; // 构造期移入 context_，此后由 worker 独占
        int debounceMs = 300;
        bool autoEmitTextReply = false;
        int historyKeep = 8; // 前缀缓存失效（重建 systemPrompt 等）后保留的近端原文条数
        std::function<std::string()> onReColdStart; // 重冷启后重建 systemPrompt 并返回新 prompt
        std::function<bool(const Outbound&)> emitOutbound;
        std::function<void(std::vector<Msg>&)> beforeLlmHook;
    };

    explicit MindLoop(MindLoopDeps deps);
    ~MindLoop();

    MindLoop(const MindLoop&) = delete;
    MindLoop& operator=(const MindLoop&) = delete;

    void stop();

    // 外部事件到达：递增代际并唤醒 worker（不投递数据，数据由 worker 从 gate 拉取）
    void notifyWake();

    void updateServices(std::shared_ptr<Llm> llm,
                        std::shared_ptr<ContextBuilder> builder,
                        std::string systemPrompt,
                        MessageSplitterConfig splitter,
                        int debounceMs,
                        bool autoEmitTextReply,
                        int historyKeep);

    // 请求在下次回合边界压缩历史（模型/端点等 Worker 侧无法自行观测的前缀变更）
    void requestCompact(const std::string& reason);

    // 标记本轮已通过 send_message 工具主动外发过消息（避免自动外发重复）
    void markMessageSentInTurn();

    // 标记本轮为「静默」：模型不想接话，但回合继续（思考/工具继续），仅抑制最终文本自动外发
    void markSilentInTurn();

    // 供工具查询当前回合的来源与频道
    MessageToolContext activeMessageContext() const;
    std::string activeChannelId() const;
    std::uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }

private:
    struct TurnServices;

    void workerLoop();
    void processTurn(GateBatch& batch, const TurnServices& snap, std::uint64_t turnGen);
    void compactHistory(int keep, const std::string& reason);

    MindLoopDeps deps_;
    std::vector<Msg> context_; // 仅 worker 线程读写，零锁
    std::string lastToolSig_;  // 上一回合的工具集签名，仅 worker 线程读写

    // 唤醒 / 代际 / 回合状态
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    bool wakePending_ = false;
    bool compactPending_ = false;
    std::string compactReason_;
    std::string activeSource_;
    std::string activeChannelId_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> sentInTurn_{false};
    std::atomic<bool> silentInTurn_{false};
};

} // namespace mio
