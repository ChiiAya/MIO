#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include "runtime/Runtime.h"

using namespace mio;
namespace {
void expect(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
class Model : public Llm {
public:
    std::function<ChatResponse(const ChatRequest&)> handler;
    ChatResponse chat(const ChatRequest& req) override { return handler(req); }
};
ChatResponse answer(const std::string& text) { ChatResponse r; r.text = text; return r; }
bool hasImage(const ChatRequest& req) {
    for (const auto& msg : req.messages)
        for (const auto& part : msg.parts) if (part.kind == Part::Kind::Image) return true;
    return false;
}
IncomingMessage input(const std::string& text, const std::string& id) {
    IncomingMessage m;
    m.conversation = ConversationKey::groupChat("20000"); m.platform = "qq";
    m.groupId = "20000"; m.senderId = "10000"; m.senderName = "User";
    m.text = text; m.messageId = id;
    return m;
}
std::shared_ptr<ConfigManager> config() {
    auto cfg = std::make_shared<AppConfig>();
    cfg->adminPort = 0;
    cfg->inputBuffer.debounceMs = 0;
    cfg->inputBuffer.maxDelayMs = 0;
    cfg->embedding.baseUrl.clear();
    cfg->embedding.maxRetries = 0;
    cfg->fusion.topicSemanticMatch = false;
    cfg->splitter.baseDelayMs = 0; cfg->splitter.delayPerCharMs = 0;
    cfg->contextBuilder.topicWarmupTurns = 0;
    return std::make_shared<ConfigManager>(cfg);
}
void mediaDelivery(const std::filesystem::path& path) {
    auto model = std::make_shared<Model>();
    Runtime runtime("Test", path, model, config());
    bool delivered = false;
    runtime.setStructuredMessageSender([&](const auto&, const auto&) { return delivered; });
    model->handler = [](const ChatRequest& req) {
        expect(hasImage(req), "initial image must reach the model"); return answer("image answer");
    };
    auto message = input("look", "1");
    message.parts.push_back({Part::Kind::Image, "data:image/png;base64,aGVsbG8=", true});
    bool failed = false;
    try { runtime.ingest(message); } catch (const std::runtime_error&) { failed = true; }
    expect(failed, "failed platform delivery must be visible");
    delivered = true;
    model->handler = [](const ChatRequest& req) {
        expect(hasImage(req), "failed delivery must leave image pending for retry");
        return answer("retried image answer");
    };
    runtime.ingest(input("try again", "2"));
    model->handler = [](const ChatRequest& req) {
        expect(!hasImage(req), "successfully answered images must not be resent"); return answer("done");
    };
    runtime.ingest(input("next", "3"));

    runtime.setStructuredMessageSender({});
    model->handler = [](const auto&) -> ChatResponse { throw std::runtime_error("model offline"); };
    message.messageId = "4";
    failed = false;
    try { runtime.ingest(message); } catch (const std::runtime_error&) { failed = true; }
    expect(failed, "model failure must reach caller");
    model->handler = [](const ChatRequest& req) {
        expect(hasImage(req), "model failure must preserve image input"); return answer("recovered");
    };
    runtime.ingest(input("retry model", "5"));
}
void contactsAndSending(const std::filesystem::path& path) {
    auto model = std::make_shared<Model>();
    Runtime runtime("Test", path, model, config());
    std::vector<std::pair<ConversationKey, OutgoingMessage>> sent;
    runtime.setStructuredMessageSender([&](const auto& conv, const auto& msg) {
        sent.emplace_back(conv, msg); return true;
    });
    int step = 0;
    model->handler = [&](const ChatRequest& req) {
        ChatResponse out;
        if (step++ == 0) {
            out.toolCalls.push_back({"contact", "upsert_contact", R"({"name":"Alice","qq":"12345","notes":"friend"})"});
        } else if (step == 2) {
            out.toolCalls.push_back({"send", "send_message", R"({"text":"hello[SEP]again","mention_user_ids":["12345"],"reply_to_message_id":"10"})"});
        } else if (step == 3) {
            out.toolCalls.push_back({"private", "send_message", R"({"text":"private hello","target":"private:30000"})"});
        } else {
            expect(req.messages.back().text.find("成功") != std::string::npos, "send result must return to model");
            return answer("done");
        }
        return out;
    };
    runtime.ingest(input("meet Alice", "10"));
    expect(sent.size() == 4, "tools and final output must reach adapter");
    expect(sent[0].second.mentionUserIds == std::vector<std::string>{"12345"} &&
           sent[0].second.replyToMessageId == "10", "first bubble must include reference and mention");
    expect(sent[1].second.mentionUserIds.empty() && sent[1].second.replyToMessageId.empty(),
           "reference/mention metadata must be attached only once");
    expect(sent[2].first == ConversationKey::privateChat("30000"), "private target must not become current sender");
    expect(runtime.systemPrompt().find("Alice") != std::string::npos &&
           runtime.systemPrompt().find("12345") != std::string::npos, "new contact must refresh Facts");
    model->handler = [](const ChatRequest& req) {
        bool mapped = false;
        for (const auto& msg : req.messages)
            if (msg.text.find("[提及对象]") != std::string::npos && msg.text.find("Alice") != std::string::npos) mapped = true;
        expect(mapped, "mention must resolve to current contact name"); return answer("mapped");
    };
    runtime.ingest(input("@12345 hi", "11"));
}
void interruptedMedia(const std::filesystem::path& path) {
    auto model = std::make_shared<Model>();
    Runtime runtime("Test", path, model, config());
    std::promise<void> started;
    auto ready = started.get_future();
    std::atomic<int> calls{0};
    model->handler = [&](const ChatRequest& req) {
        if (calls.fetch_add(1) == 0) {
            expect(hasImage(req), "first interrupted turn must contain image");
            started.set_value();
            ChatResponse r;
            r.toolCalls.push_back({"wait", "wait", R"({"seconds":60})"});
            return r;
        }
        expect(hasImage(req), "new generation must still see interrupted image");
        return answer("new answer");
    };
    auto message = input("look", "20");
    message.parts.push_back({Part::Kind::Image, "data:image/png;base64,aGVsbG8=", true});
    auto first = std::async(std::launch::async, [&] { return runtime.ingest(message); });
    expect(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "model must begin first turn");
    const auto second = runtime.ingest(input("also explain this", "21"));
    expect(first.get().interrupted, "superseded turn must be interrupted");
    expect(!second.interrupted && second.text == "new answer", "latest turn must answer both inputs");
}
} // namespace
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("mio-runtime-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    mediaDelivery(root / "media");
    contactsAndSending(root / "contacts");
    interruptedMedia(root / "interruption");
    std::filesystem::remove_all(root);
    std::cout << "Runtime integration tests passed\n";
}
