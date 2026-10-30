#include "runtime/MessageTools.h"

#include <set>

namespace mio {

void registerMessageTools(ToolRegistry& registry,
                          std::function<MessageToolContext()> context,
                          OutboundSender send) {
    ToolDef def;
    def.name = "send_message";
    def.description =
        "向指定频道或当前会话发送消息。text 中可用 [SEP] 分气泡；"
        "如需 @ 群成员请填写 mention_user_ids，引用消息请填 reply_to_message_id。";
    def.parametersJsonSchema = {
        {"type", "object"},
        {"properties", {
            {"text", {{"type", "string"}, {"description", "消息正文"}}},
            {"target", {{"type", "string"}, {"description", "目标 channelId（如 group:群号 或 private:账号），留空默认当前会话"}}},
            {"source", {{"type", "string"}, {"description", "可选目标平台源（如 napcat / console），留空默认当前事件源"}}},
            {"mention_user_ids", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "需要 @ 的用户 ID 列表"}}},
            {"reply_to_message_id", {{"type", "string"}, {"description", "要引用的消息 ID"}}}
        }},
        {"required", nlohmann::json::array({"text"})}
    };

    registry.add(std::move(def),
        [context = std::move(context), send = std::move(send)](const nlohmann::json& args) -> std::string {
            const auto active = context();
            Outbound out;
            out.text = args.value("text", "");
            if (out.text.empty() || out.text.find_first_not_of(" \t\r\n") == std::string::npos) {
                return "error: 发送内容 text 不能为空";
            }

            out.source = args.value("source", active.source);
            out.channelId = args.value("target", "");
            if (out.channelId.empty()) {
                out.channelId = active.channelId;
            }
            if (out.channelId.empty()) {
                return "error: 缺少目标 channelId（请指定 target 参数）";
            }

            out.replyTo = args.value("reply_to_message_id", "");
            if (args.contains("mention_user_ids") && args["mention_user_ids"].is_array()) {
                std::set<std::string> seen;
                for (const auto& v : args["mention_user_ids"]) {
                    if (v.is_string() && !v.get<std::string>().empty()) {
                        auto id = v.get<std::string>();
                        if (seen.insert(id).second) {
                            out.mentions.push_back(std::move(id));
                        }
                    }
                }
            }

            if (!send || !send(out)) {
                return "error: 消息发送失败或中途停止";
            }
            return "消息发送成功。";
        }, ToolLayer::Builtin);
}

} // namespace mio
