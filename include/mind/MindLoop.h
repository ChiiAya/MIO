#pragma once
// ============================================================================
// MindLoop —— 单意识流工作循环引擎
//
// 职责：
//   1. 维护全局唯一意识流历史 `context_`（Worker 线程独占，单向流转零锁）；
//   2. 异步信箱消费者：从 `inbox_` 取出回合任务排队处理；
//   3. 驱动 `ContextBuilder` + `ToolLoop` 并通过 `emitOutbound` 完成出站回复；
//   4. 维护当前活跃的回合来源与目标频道（供消息工具查询上下文）。
// ============================================================================

#include "config/AppConfig.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "core/event/AttentionGate.h"
#include "core/message/Message.h"
#include "providers/llm/Llm.h"
#include "providers/llm/tool/ToolRegistry.h"
#include "runtime/MessageTools.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mio {

struct TurnInput {
    std::vector<Msg> msgs;
    std::string source;
    std::string channelId;
};

class MindLoop {
public:
    struct MindLoopDeps {
        AttentionGate& gate;
        ToolRegistry& registry;
        std::shared_ptr<Llm> llm;
        std::shared_ptr<ContextBuilder> builder;
        std::string systemPrompt;
        MessageSplitterConfig splitter;

        std::function<bool(const Outbound&)> emitOutbound;
        std::function<void(std::vector<Msg>&)> beforeLlmHook;
    };

    explicit MindLoop(MindLoopDeps deps);
    ~MindLoop();

    MindLoop(const MindLoop&) = delete;
    MindLoop& operator=(const MindLoop&) = delete;

    void postTurn(TurnInput input);

    void start();
    void stop();

    void updateServices(std::shared_ptr<Llm> llm,
                        std::shared_ptr<ContextBuilder> builder,
                        std::string systemPrompt,
                        MessageSplitterConfig splitter);

    // 运行隐藏维护 prompt（例如定时整理，不向外部平台发消息）
    void runMaintenancePrompt(const std::string& prompt);

    // 记忆流读取与冷启动填充
    void setHistory(std::vector<Msg> msgs);
    const std::vector<Msg>& history() const { return context_; }

    // 供工具查询当前对话的来源与频道
    MessageToolContext activeMessageContext() const;
    std::string activeChannelId() const;
    std::uint64_t generation() const { return generation_; }

private:
    void workerLoop();
    void processTurn(TurnInput input);

    MindLoopDeps deps_;
    std::vector<Msg> context_;

    // 信箱与线程同步
    std::mutex inboxMtx_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stop_ = false;

    std::vector<TurnInput> inbox_;

    // 活跃状态
    std::string activeSource_;
    std::string activeChannelId_;
    std::uint64_t generation_ = 0;
};

} // namespace mio
