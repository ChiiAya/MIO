#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mio {

enum class Role {
    System,
    User,
    Assistant,
    Tool,
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string argumentsJson;
};

// 多模态附件分片（供全模态模型传入图片、音频、视频、文件等；纯文本直接走 Msg::text）
struct Part {
    enum class Kind {
        Image,
        Audio,
        Video,
        File,
    };
    Kind kind = Kind::Image;
    std::string data;     // URL、base64 或 data-uri
    std::string mimeType; // 可选格式描述，如 "image/jpeg", "wav", "mp4"
};

const char* partKindToString(Part::Kind k);
Part::Kind partKindFromString(const std::string& s);

// Msg 仅作为送往 Context / LLM 的标准消息结构
struct Msg {
    Role role = Role::User;

    std::string text;
    std::string reasoningContent;
    std::vector<Part> parts;         // 多模态媒体分片（为空表示纯文本）
    std::vector<ToolCall> toolCalls;
    std::string toolCallId;

    bool isTextOnly() const { return parts.empty(); }
};

// 统一出站消息语义（取代旧的 ConversationKey + OutgoingMessage）
struct Outbound {
    std::string source;                  // 目标适配器源，如 "napcat", "console"
    std::string channelId;               // 目标通道，如 "group:123456", "private:7890"
    std::string text;                    // 单气泡或待拆分文本
    std::string replyTo;                 // 可选引用消息 ID
    std::vector<std::string> mentions;   // 可选 @ 用户 ID 列表
    std::vector<Part> parts;             // 可选多模态附件
};

nlohmann::json msgToJson(const Msg& m);
Msg msgFromJson(const nlohmann::json& j);

} // namespace mio
