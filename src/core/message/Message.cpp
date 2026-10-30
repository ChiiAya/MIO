#include "core/message/Message.h"

namespace mio {

namespace {

const char* roleToString(Role r) {
    switch (r) {
    case Role::System:    return "system";
    case Role::User:      return "user";
    case Role::Assistant: return "assistant";
    case Role::Tool:      return "tool";
    }
    return "user";
}

Role roleFromString(const std::string& s) {
    if (s == "system")    return Role::System;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool")      return Role::Tool;
    return Role::User;
}

} // namespace

const char* partKindToString(Part::Kind k) {
    switch (k) {
    case Part::Kind::Image: return "image";
    case Part::Kind::Audio: return "audio";
    case Part::Kind::Video: return "video";
    case Part::Kind::File:  return "file";
    }
    return "image";
}

Part::Kind partKindFromString(const std::string& s) {
    if (s == "audio") return Part::Kind::Audio;
    if (s == "video") return Part::Kind::Video;
    if (s == "file")  return Part::Kind::File;
    return Part::Kind::Image;
}

nlohmann::json msgToJson(const Msg& m) {
    nlohmann::json j = {{"role", roleToString(m.role)}};

    if (!m.text.empty())             j["text"] = m.text;
    if (!m.reasoningContent.empty()) j["reasoningContent"] = m.reasoningContent;
    if (!m.toolCallId.empty())       j["toolCallId"] = m.toolCallId;

    if (!m.parts.empty()) {
        auto arr = nlohmann::json::array();
        for (const auto& p : m.parts) {
            nlohmann::json pj = {
                {"kind", partKindToString(p.kind)},
                {"data", p.data},
            };
            if (!p.mimeType.empty()) pj["mimeType"] = p.mimeType;
            arr.push_back(std::move(pj));
        }
        j["parts"] = std::move(arr);
    }

    if (!m.toolCalls.empty()) {
        auto arr = nlohmann::json::array();
        for (const auto& tc : m.toolCalls) {
            arr.push_back({
                {"id", tc.id},
                {"name", tc.name},
                {"argumentsJson", tc.argumentsJson},
            });
        }
        j["toolCalls"] = std::move(arr);
    }

    return j;
}

Msg msgFromJson(const nlohmann::json& j) {
    Msg m;
    m.role = roleFromString(j.value("role", "user"));
    m.text = j.value("text", "");
    m.toolCallId = j.value("toolCallId", "");

    if (auto it = j.find("reasoningContent"); it != j.end() && it->is_string()) {
        m.reasoningContent = it->get<std::string>();
    } else if (auto it2 = j.find("reasoning_content"); it2 != j.end() && it2->is_string()) {
        m.reasoningContent = it2->get<std::string>();
    }

    if (auto it = j.find("parts"); it != j.end() && it->is_array()) {
        for (const auto& p : *it) {
            m.parts.push_back(Part{
                partKindFromString(p.value("kind", "image")),
                p.value("data", p.value("text", "")),
                p.value("mimeType", ""),
            });
        }
    }

    if (auto it = j.find("toolCalls"); it != j.end() && it->is_array()) {
        for (const auto& tc : *it) {
            m.toolCalls.push_back(ToolCall{
                tc.value("id", ""),
                tc.value("name", ""),
                tc.value("argumentsJson", "{}"),
            });
        }
    }

    return m;
}

} // namespace mio
