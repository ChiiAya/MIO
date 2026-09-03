// ============================================================================
// 消息的内、外格式转换 —— 持久化用
//
// 内部落盘格式（自有）和 OpenAI wire 格式的区别：
//   {"role":"user","text":"你好"}                              ← 纯文本快捷路径
//   {"role":"user","parts":[{"kind":"text","text":"看看"},{"kind":"image","text":"data:..."}]}
//   {"role":"assistant","reasoningContent":"...","toolCalls":[{"id":"call_1","name":"t","argumentsJson":"{}"}]}
//   {"role":"tool","toolCallId":"call_1","text":"42"}
//
// 约定：
//   1. ephemeral 的 part 绝不落盘 —— ChatArchive 之外这里是第二道防线；
//   2. 纯文本消息不转成 parts 数组再存，避免体积膨胀；
//   3. assistant 带 toolCalls 时 text 允许为空，但字段仍要写出空串，
//      保证"没写的字段 = 不存在"这条不变量。
//   4. senderId/platform 仅 User 消息携带（来源身份，wire 层渲染为标签）；
//      旧历史文件没有这两个字段，载入后即为空、不打标签。
// ============================================================================

#include "core/message/Message.h"

namespace mio {

namespace {

const char* roleToString(Role r) {
    switch (r) {
    case Role::System: return "system";
    case Role::User: return "user";
    case Role::Assistant: return "assistant";
    case Role::Tool: return "tool";
    }
    return "user";
}

Role roleFromString(const std::string& s) {
    if (s == "system") return Role::System;
    if (s == "assistant") return Role::Assistant;
    if (s == "tool") return Role::Tool;
    return Role::User;
}

} // namespace

nlohmann::json msgToJson(const Msg& m) {
    nlohmann::json j;
    j["role"] = roleToString(m.role);

    // 只保留非 ephemeral 的 parts（正常情况下 ephemeral 已在发送前被剥离，
    // 这里是双保险）
    std::vector<Part> keptParts;
    for (const auto& p : m.parts)
        if (!p.ephemeral) keptParts.push_back(p);

    // 存储形态规范化：剔除后若只剩一个纯文本 part，则折回普通 text 字段。
    // 不做这步的话，历史文件会逐渐被 "{parts:[一个元素]}" 占满，
    // 载入后走多模态序列化路径平白多花 token。
    if (keptParts.size() == 1 &&
        keptParts[0].kind == Part::Kind::Text &&
        m.toolCalls.empty()) {
        j["text"] = keptParts[0].text.empty() ? m.text : keptParts[0].text;
    } else {
        if (!keptParts.empty()) {
            auto arr = nlohmann::json::array();
            for (const auto& p : keptParts) {
                arr.push_back({
                    {"kind", p.kind == Part::Kind::Image ? "image" : "text"},
                    {"text", p.text},
                });
            }
            j["parts"] = std::move(arr);
        }
        if (!m.text.empty()) j["text"] = m.text;
    }

    if (!m.reasoningContent.empty()) j["reasoningContent"] = m.reasoningContent;

    if (!m.toolCalls.empty()) {
        auto arr = nlohmann::json::array();
        for (const auto& tc : m.toolCalls) {
            arr.push_back({{"id", tc.id},
                           {"name", tc.name},
                           {"argumentsJson", tc.argumentsJson}});
        }
        j["toolCalls"] = std::move(arr);
    }
    if (!m.toolCallId.empty()) j["toolCallId"] = m.toolCallId;
    if (m.createdAt > 0) j["createdAt"] = m.createdAt;
    if (m.isSummary) j["summary"] = true;
    if (m.firstEncounter) j["firstEncounter"] = true;
    if (!m.senderId.empty()) j["senderId"] = m.senderId;
    if (!m.senderName.empty()) j["senderName"] = m.senderName;
    if (!m.platform.empty()) j["platform"] = m.platform;
    if (!m.groupId.empty()) j["groupId"] = m.groupId;

    return j;
}

Msg msgFromJson(const nlohmann::json& j) {
    Msg m;
    m.role = roleFromString(j.value("role", "user"));

    if (auto it = j.find("parts"); it != j.end() && it->is_array()) {
        for (const auto& p : *it) {
            Part part;
            part.kind =
                p.value("kind", "text") == "image" ? Part::Kind::Image : Part::Kind::Text;
            part.text = p.value("text", "");
            m.parts.push_back(std::move(part));
        }
    }

    m.text = j.value("text", "");
    if (auto it = j.find("reasoningContent"); it != j.end() && it->is_string()) {
        m.reasoningContent = it->get<std::string>();
    } else if (auto it = j.find("reasoning_content");
               it != j.end() && it->is_string()) {
        m.reasoningContent = it->get<std::string>();
    }

    if (auto it = j.find("toolCalls"); it != j.end() && it->is_array()) {
        for (const auto& tc : *it) {
            ToolCall call;
            call.id = tc.value("id", "");
            call.name = tc.value("name", "");
            call.argumentsJson = tc.value("argumentsJson", "{}");
            m.toolCalls.push_back(std::move(call));
        }
    }
    m.toolCallId = j.value("toolCallId", "");
    m.createdAt = j.value("createdAt", std::int64_t{0});
    m.isSummary = j.value("summary", false);
    m.firstEncounter = j.value("firstEncounter", false);
    m.senderId = j.value("senderId", "");
    m.senderName = j.value("senderName", "");
    m.platform = j.value("platform", "");
    m.groupId = j.value("groupId", "");

    return m;
}

} // namespace mio
