#pragma once
// ============================================================================
// 工具调用循环（Agent Loop）
//
//     while (没到步数上限):
//         resp = llm.chat(请求)
//         if 没有工具调用: 返回
//         for each tool_call: 执行 -> 结果以 role=tool 消息追加回上下文
// ============================================================================

#include <functional>

#include "providers/llm/Llm.h"
#include "providers/llm/tool/ToolRegistry.h"

namespace mio {

struct ToolLoopOptions {
    int maxSteps = 8;   // 一次提问最多几轮"模型<->工具"往返；闲聊 bot 用不了太多
    std::size_t maxInlineResultBytes = 4096; // 工具结果超过这个字节数就落盘截断
    int repeatStreakThreshold = 3;           // 同名同参连续调用 N 次后注入提醒
    std::filesystem::path overflowDir = "data/tool_overflow";
};

// 事件回调：循环内产生的每条新消息（assistant / tool）都会推给 sink，
// 调用方用它做持久化、前端推送等。可为空函数。
using MessageSink = std::function<void(const Msg&)>;

ChatResponse runToolLoop(Llm& llm, ToolRegistry& tools, ChatRequest req,
                         const ToolLoopOptions& opt = {},
                         const MessageSink& sink = {});

} // namespace mio
