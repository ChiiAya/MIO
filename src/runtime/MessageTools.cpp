#include "runtime/MessageTools.h"

#include <algorithm>
#include <set>

namespace mio {
namespace {
bool numericId(const std::string& value, bool signedId = false) {
    const auto begin = signedId && !value.empty() && value.front() == '-' ? 1u : 0u;
    return value.size() > begin && value.size() <= 20 &&
           std::all_of(value.begin() + begin, value.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}
} // namespace

void registerMessageTools(ToolRegistry& registry,
                          std::function<MessageToolContext()> context,
                          StructuredMessageSender send) {
    ToolDef def;
    def.name = "send_message";
    def.description =
        "发送消息，text 中可用 [SEP] 分气泡。QQ 群聊 @ 请填写 mention_user_ids，"
        "引用请填当前会话消息标签中的 reply_to_message_id；不要在 text 中编写 CQ 码。";
    def.parametersJsonSchema = {
        {"type", "object"},
        {"properties", {
            {"text", {{"type", "string"}, {"description", "消息正文"}}},
            {"target", {{"type", "string"}, {"description", "留空为当前会话；其他目标用 private:QQ 或 group:群号。裸ID沿用当前会话类型"}}},
            {"mention_user_ids", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "需要 @ 的 QQ 号，可先查通讯录；不支持 @全体"}}},
            {"reply_to_message_id", {{"type", "string"}, {"description", "当前会话中要引用的 message_id"}}}
        }},
        {"required", nlohmann::json::array({"text"})}
    };
    registry.add(std::move(def),
        [context = std::move(context), send = std::move(send)](const nlohmann::json& args) {
            const auto active = context();
            auto target = active.conversation;
            const std::string requested = args.value("target", "");
            if (!requested.empty()) {
                if (requested.rfind("group:", 0) == 0)
                    target = ConversationKey::groupChat(requested.substr(6));
                else if (requested.rfind("private:", 0) == 0)
                    target = ConversationKey::privateChat(requested.substr(8));
                else
                    target.id = requested;
            }
            OutgoingMessage message;
            message.text = args.value("text", "");
            if (message.text.empty() || message.text.find_first_not_of(" \t\r\n") == std::string::npos)
                return std::string("error: 发送内容 text 不能为空");
            const bool qq = active.platform == "qq" || active.platform == "napcat" || active.platform == "onebot";
            if (target.id.empty() || (qq && !numericId(target.id)))
                return std::string("error: 目标ID无效");
            message.replyToMessageId = args.value("reply_to_message_id", "");
            if (!message.replyToMessageId.empty() &&
                (!qq || !(target == active.conversation) || !numericId(message.replyToMessageId, true) ||
                 std::find(active.replyableMessageIds.begin(), active.replyableMessageIds.end(),
                           message.replyToMessageId) == active.replyableMessageIds.end()))
                return std::string("error: 引用必须使用当前 QQ 会话的有效消息ID");
            if (args.contains("mention_user_ids")) {
                if (!args["mention_user_ids"].is_array())
                    return std::string("error: mention_user_ids 必须是QQ号字符串数组");
                std::set<std::string> seen;
                for (const auto& value : args["mention_user_ids"]) {
                    if (!value.is_string() || !numericId(value.get<std::string>()))
                        return std::string("error: @对象必须为QQ号，不支持全体或昵称");
                    auto id = value.get<std::string>();
                    if (seen.insert(id).second) message.mentionUserIds.push_back(std::move(id));
                }
            }
            if (!message.mentionUserIds.empty() && (!qq || target.scope != ConversationScope::Group))
                return std::string("error: @消息仅支持QQ 群聊");
            if (!send || !send(target, message))
                return std::string("error: 消息发送失败或中途停止，请勿声称发送成功；部分气泡可能已经送达");
            return std::string("消息发送成功。");
        }, ToolLayer::Builtin);
}
} // namespace mio
