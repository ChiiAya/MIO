#pragma once
// ============================================================================
// MIO 统一消息模型
//   * text 走快捷路径 —— 聊天场景绝大多数是纯文本，没必要为了一句"你好"
//     构造 content parts 数组（这正是 OpenAI wire 格式的开销来源之一）。
//   * Part.ephemeral —— 只发给模型、不写入历史的"临时注入"。用于 RAG
//     检索结果等动态信息。
//   * 助手带 toolCalls 时允许 text 为空；tool 消息必须带 toolCallId。
//   * createdAt —— 消息时间戳（epoch 秒），时间感知与冷热判定依据；
//   * isSummary —— 首条滚动摘要标记（仅装配器消费，wire 以 system 上送）；
//   * firstEncounter —— 平台标记"首次见到该用户"（渲染为初识提示）。
//
// reasoningContent —— 助手消息的思维链/推理内容（wire 字段为
// reasoning_content）。DeepSeek / MiMo 等推理系模型在历史中回传 assistant
// 消息（尤其是带 tool_calls 的 assistant 消息）时要求原样带回
// reasoning_content，因此它需要随消息一起持久化，并在 toWireMessages 中
// 渲染成 OpenAI 兼容格式。
// ============================================================================

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mio {

enum class Role {
    System,    // 人设/规则
    User,      // 用户
    Assistant, // 模型输出，可携带 toolCalls
    Tool,      // 工具执行结果
};

struct ToolCall {
    std::string id;            // 本次调用唯一 id，由模型生成，回传时原样引用
    std::string name;
    std::string argumentsJson; // OpenAI 协议里这是 JSON **字符串**，不是对象
};

struct Part {
    enum class Kind {
        Text,
        Image, // text 字段此时存 URL 或 data:image/...;base64,... 的 data-uri
    };
    Kind kind = Kind::Text;
    std::string text;
    bool ephemeral = false; // true = 仅发送给模型本轮，不持久化
};

struct Msg {
    Role role = Role::User;

    std::string text;                // 主文本
    std::string reasoningContent;    // 助手思维链/推理内容（wire: reasoning_content）
    std::vector<Part> parts;         // 非空时按多模态序列化（wire 层）
    std::vector<ToolCall> toolCalls; // 仅 Assistant 使用
    std::string toolCallId;          // 仅 Tool 使用
    std::int64_t createdAt = 0;      // epoch 秒；0 = 未知（旧数据）
    bool isSummary = false;          // 首条滚动摘要（历史首条）
    bool firstEncounter = false;     // 平台标记：首次见到该用户

    // 消息来源身份（仅 User 携带，来自平台事件；模型回复/工具结果为空）。
    // wire 层渲染为 [平台][private]/[平台][group:群号] + [sender:人] 前缀标签；
    // groupId 仅群聊消息携带；senderName = Router 注入的显示名（图谱昵称，
    // 未命中为空，渲染时回退 senderId）。senderId 始终存原始平台 id，
    // 供档案冷读取/身份归一使用。
    std::string senderId;
    std::string senderName;
    std::string platform;
    std::string groupId;

    bool isTextOnly() const { return parts.empty(); }
};

// ---- 内部持久化格式（自有格式，稳定可版本化）---------------------------------
// 与 wire 格式分开写：内部格式我们说了算（未来加字段、做迁移都自由），
// wire 格式由各适配器负责翻译。
nlohmann::json msgToJson(const Msg& m);            // 用于历史落盘
Msg msgFromJson(const nlohmann::json& j);          // 用于历史加载

} // namespace mio
