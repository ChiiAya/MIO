#include <iostream>
#include <stdexcept>
#include "runtime/MessageTools.h"
#include "context/inputBuffer/inputBuffer.h"
#include "providers/llm/tool/ToolLoop.h"

using namespace mio;
namespace {
void expect(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
class ToolModel : public Llm {
    int step_ = 0;
public:
    ChatResponse chat(const ChatRequest& request) override {
        ChatResponse out;
        if (step_++ == 0) {
            out.toolCalls.push_back({"send", "send_message", R"({"text":"hello","mention_user_ids":["12345"],"reply_to_message_id":"-55"})"});
        } else {
            expect(request.messages.back().text.find("成功") != std::string::npos,
                   "send tool result must reach model");
            out.text = "done";
        }
        return out;
    }
};
}
int main() {
    ToolRegistry registry;
    auto active = ConversationKey::groupChat("20000");
    ConversationKey target;
    OutgoingMessage sent;
    bool success = true;
    int calls = 0;
    registerMessageTools(registry, [&] { return MessageToolContext{active, "qq", {"-55", "55"}}; },
        [&](const ConversationKey& conv, const OutgoingMessage& message) {
            ++calls; target = conv; sent = message; return success;
        });
    ToolModel model;
    ChatRequest request;
    request.tools = registry.defs();
    const auto response = runToolLoop(model, registry, request);
    expect(response.text == "done" && calls == 1, "LLM must invoke structured send tool");
    expect(sent.mentionUserIds == std::vector<std::string>{"12345"} && sent.replyToMessageId == "-55",
           "argument filtering must preserve mention and reply metadata");
    registry.invoke("send_message", {{"text", "private"}, {"target", "private:30000"}});
    expect(target == ConversationKey::privateChat("30000"), "explicit private target must be respected");
    expect(sent.replyToMessageId.empty(), "plain send must not inherit a previous reference");
    success = false;
    expect(registry.invoke("send_message", {{"text", "failed"}}).rfind("error:", 0) == 0,
           "sender failure must not be reported as success");
    const int previousCalls = calls;
    expect(registry.invoke("send_message", {{"text", "bad"}, {"target", "private:30000"}, {"reply_to_message_id", "55"}}).rfind("error:", 0) == 0,
           "references must not cross conversations");
    expect(registry.invoke("send_message", {{"text", "bad"}, {"mention_user_ids", {"all"}}}).rfind("error:", 0) == 0,
           "invalid mention must be rejected");
    expect(calls == previousCalls, "invalid messages must not be delivered");
    expect(registry.invoke("send_message", {{"text", "bad"}, {"reply_to_message_id", "123456"}}).rfind("error:", 0) == 0,
           "invented references must be rejected");

    InputBufferConfig config;
    config.debounceMs = 0;
    InputBuffer buffer(config);
    IncomingMessage message;
    message.conversation = active; message.senderId = "10000"; message.platform = "qq";
    message.text = "question"; message.messageId = "91";
    message.replyToMessageId = "90"; message.atUserIds = {"12345"};
    buffer.push(message, "Alice", 1);
    message.messageId = "92";
    buffer.push(message, "Alice", 2);
    const auto batch = buffer.waitForBatch();
    expect(batch.turns.size() == 2, "messages with IDs must retain separate reference targets");
    const auto restored = msgFromJson(msgToJson(batch.turns.front()));
    expect(restored.messageId == "91" && restored.replyToMessageId == "90" &&
           restored.atUserIds == std::vector<std::string>{"12345"},
           "archive reload must preserve message metadata");
    std::cout << "delivery regression tests passed\n";
}
