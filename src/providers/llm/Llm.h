#pragma once
// ============================================================================
// LLM 抽象接口 + 请求/响应契约
//
// 核心原则：接口只暴露"语义"，不暴露任何厂商方言 ——
//   * 上游（工具循环、平台层）永远不知道 OpenAI 是什么样子；
//   * 所有厂商怪癖（比如 DeepSeek/MiMo 要求回传 reasoning_content）都被关在
//     各个适配器内部。
//
// 现阶段只需要一个实现：OpenAiCompat（OpenAI Chat Completions 兼容协议），
// 它能覆盖 DeepSeek / Qwen / GLM / Kimi / SiliconFlow / vLLM / llama.cpp /
// LM Studio / Ollama(/v1) 等几乎全部兼容端点。
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/message/Message.h"

namespace mio {

struct ToolDef {
    std::string name;
    std::string description;
    // 直接存 JSON Schema 对象（{"type":"object","properties":{...}}），
    // 不发明自己的 schema DSL —— JSON Schema 本身就是通用语言。
    nlohmann::json parametersJsonSchema;
    bool isTerminal = false; // 终结性工具（如 keepsilent），调用后立即结束工具循环，不进行无谓二次 LLM 请求
};

struct ChatRequest {
    std::string systemPrompt;      // 每次请求整体重发；内容必须稳定（见 mind/Facts.h）
    std::vector<Msg> messages;     // 完整对话历史（不含 system）
    std::vector<ToolDef> tools;    // 为空 = 本次请求不给模型开工具
    std::string modelOverride;     // 空 = 用适配器配置里的默认模型
};

struct Usage {
    std::int64_t inputOther = 0;   // 输入 token，其中未命中缓存的 部分
    std::int64_t inputCached = 0;  // 命中前缀缓存的输入 token —— 低成本的关键指标
    std::int64_t output = 0;

    std::int64_t total() const { return inputOther + inputCached + output; }
};

struct ChatResponse {
    std::string text;                  // 正文（可为空，例如只有 tool_calls 时）
    std::string reasoning;             // 思维链输出
    std::vector<ToolCall> toolCalls;
    std::string finishReason;          // stop / tool_calls / length ...
    Usage usage;
    nlohmann::json raw;                // 原始响应。调试、对账、未来的高级功能都用它
};

class Llm {
public:
    virtual ~Llm() = default;

    // 同步阻塞调用一次补全。失败抛 std::runtime_error。
    // 流式版 chatStream(req, sink) 等 MIO 需要打字机效果/多段发送时再加，
    // 不要提前把它塞进同一个虚方法 —— v1 非流式更简单也更省电费。
    virtual ChatResponse chat(const ChatRequest& req) = 0;
};

} // namespace mio
