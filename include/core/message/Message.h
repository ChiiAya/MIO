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

nlohmann::json msgToJson(const Msg& m);
Msg msgFromJson(const nlohmann::json& j);

} // namespace mio
