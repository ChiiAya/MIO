#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "context/achieve/Achieve.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "providers/llm/tool/ToolLoop.h"

using namespace mio;

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Msg text(Role role, const std::string& value) {
    Msg msg{role};
    msg.text = value;
    return msg;
}

Msg call(const std::string& id) {
    Msg msg{Role::Assistant};
    msg.toolCalls.push_back({id, "test_tool", "{}"});
    return msg;
}

Msg result(const std::string& id) {
    Msg msg = text(Role::Tool, "ok");
    msg.toolCallId = id;
    return msg;
}

Msg imageMessage() {
    Msg msg = text(Role::User, "inspect this image");
    msg.parts.push_back({Part::Kind::Image, "data:image/png;base64,test", true});
    return msg;
}

bool hasImage(const std::vector<Msg>& messages) {
    for (const auto& msg : messages)
        for (const auto& part : msg.parts)
            if (part.kind == Part::Kind::Image) return true;
    return false;
}

void expectValidTools(const std::vector<Msg>& messages) {
    std::set<std::string> pending;
    for (const auto& msg : messages) {
        if (msg.role == Role::Tool) {
            expect(pending.erase(msg.toolCallId) == 1, "orphan or duplicate tool result");
        } else {
            expect(pending.empty(), "tool call missing result before next message");
            if (msg.role == Role::Assistant)
                for (const auto& tc : msg.toolCalls) pending.insert(tc.id);
        }
    }
    expect(pending.empty(), "tool call missing result at end of history");
}

class FakeLlm : public Llm {
public:
    std::function<ChatResponse(const ChatRequest&)> respond;
    ChatResponse chat(const ChatRequest& request) override {
        if (respond) return respond(request);
        ChatResponse response;
        response.text = "summary\n话题：图像识别\n私密性判断：私密";
        return response;
    }
};

struct TestArchive {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("mio-lifecycle-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    Achieve archive{path};
    ~TestArchive() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec))
            std::filesystem::remove(entry.path(), ec);
        std::filesystem::remove(path, ec);
    }
};

std::vector<Msg> completedHistory() {
    return {text(Role::User, "old"), text(Role::Assistant, "old answer"),
            text(Role::User, "new"), call("a"), result("a"),
            call("b"), result("b"), text(Role::Assistant, "done")};
}

void coldReadKeepsToolPairs() {
    TestArchive storage;
    const auto key = ConversationKey::privateChat("test");
    storage.archive.append(key, completedHistory());
    auto llm = std::make_shared<FakeLlm>();
    SummaryManager summary(800, llm);
    const auto cold = storage.archive.coldRead(key, summary, 20, 4);
    expectValidTools(cold.msgs);
    expect(cold.msgs.back().text == "done", "latest response lost");
}

void compressionKeepsToolPairs() {
    auto llm = std::make_shared<FakeLlm>();
    SummaryManager summary(800, llm);
    auto history = completedHistory();
    history.insert(history.end() - 1, result("orphan"));
    FusionUnit unit(ConversationKey::privateChat("test"), history);
    unit.reColdStart(summary);
    expectValidTools(unit.context());
    const auto redirected = unit.beRedirected();
    expectValidTools(redirected);
}

void interruptedHistoryIsRepaired() {
    TestArchive storage;
    const auto key = ConversationKey::privateChat("interrupted");
    auto incomplete = call("a");
    incomplete.toolCalls.push_back({"b", "test_tool", "{}"});
    storage.archive.append(key, {text(Role::User, "question"), incomplete, result("a")});
    SummaryManager summary(800, std::make_shared<FakeLlm>());
    const auto cold = storage.archive.coldRead(key, summary);
    expectValidTools(cold.msgs);
    expect(cold.msgs.back().toolCallId == "b", "missing cancelled tool result");
}

void summaryParsesUtf8Markers() {
    auto llm = std::make_shared<FakeLlm>();
    SummaryManager summary(800, llm);
    auto outcome = summary.summarizeWithVerdict({text(Role::User, "test")});
    expect(outcome.topic == "图像识别", "full-width colon corrupted UTF-8 topic");
    expect(outcome.privateVerdict, "private verdict lost");
    expect(!nlohmann::json(outcome.topic).dump().empty(), "topic cannot be serialized");
    llm->respond = [](const ChatRequest&) {
        ChatResponse r;
        r.text = "summary\n私密性判断: 公开\n话题: test topic";
        return r;
    };
    outcome = summary.summarizeWithVerdict({text(Role::User, "test")});
    expect(outcome.topic == "test topic" && !outcome.privateVerdict,
           "ASCII colon or marker order not supported");
}

void interruptedImageSurvivesBuildAndCompression() {
    std::vector<Msg> history{text(Role::User, "old"), text(Role::Assistant, "done"), imageMessage()};
    for (int i = 0; i < 4; ++i) {
        history.push_back(call(std::to_string(i)));
        history.push_back(result(std::to_string(i)));
    }
    history.push_back(text(Role::User, "please continue"));
    FusionUnit unit(ConversationKey::privateChat("test"), history);
    auto summary = std::make_shared<SummaryManager>(800, std::make_shared<FakeLlm>());
    ContextBuilderConfig cfg;
    cfg.budgetTokens = 1;
    ContextBuilder builder(cfg, summary);
    const auto built = builder.build({"", &unit, 0});
    expect(hasImage(built.request.messages), "unanswered image lost during compression");
    expect(hasImage(unit.context()), "unanswered image removed from unit");
    expectValidTools(built.request.messages);
}

void toolCallsDoNotConsumeImages() {
    FusionUnit unit(ConversationKey::privateChat("test"),
        {imageMessage(), call("a"), result("a"), text(Role::User, "continue")});
    ContextBuilder builder({}, nullptr);
    expect(hasImage(builder.build({"", &unit, 0}).request.messages),
           "tool-call assistant treated as a completed response");
    unit.append(text(Role::Assistant, "image described"));
    expect(!hasImage(builder.build({"", &unit, 0}).request.messages),
           "completed historical image sent again");
}

void interruptedToolBatchGetsResults() {
    FakeLlm llm;
    llm.respond = [](const ChatRequest&) {
        ChatResponse response;
        response.toolCalls = {{"a", "test_tool", "{}"}, {"b", "test_tool", "{}"}};
        return response;
    };
    ToolRegistry registry;
    bool interrupted = false;
    int executed = 0;
    registry.add({"test_tool", "test", {{"type", "object"}, {"properties", nlohmann::json::object()}}},
                 [&](const nlohmann::json&) {
                     ++executed;
                     interrupted = true;
                     return "ok";
                 });
    ToolLoopOptions options;
    options.shouldInterrupt = [&] { return interrupted; };
    std::vector<Msg> saved;
    const auto response = runToolLoop(llm, registry, {}, options,
                                     [&](const Msg& msg) { saved.push_back(msg); });
    expect(response.finishReason == "interrupted", "interruption not returned");
    expect(executed == 1, "cancelled tool was executed");
    expectValidTools(saved);
    expect(saved.back().toolCallId == "b", "cancelled result not persisted");
}

void terminalAndFallbackCompleteInput() {
    FakeLlm llm;
    llm.respond = [](const ChatRequest&) {
        ChatResponse r; r.toolCalls = {{"a", "silent", "{}"}}; return r;
    };
    ToolRegistry registry;
    registry.add({"silent", "", {{"type", "object"}, {"properties", nlohmann::json::object()}}, true},
                 [](const auto&) { return "kept silent"; });
    std::vector<Msg> saved{imageMessage()};
    runToolLoop(llm, registry, {}, {}, [&](const Msg& m) { saved.push_back(m); });
    FusionUnit unit(ConversationKey::privateChat("test"), saved);
    ContextBuilder builder({}, nullptr);
    expect(!hasImage(builder.build({"", &unit, 0}).request.messages), "silent input must be complete");
    expect(!saved.back().text.empty(), "completion must not serialize as empty assistant");

    ToolLoopOptions options; options.maxSteps = 0;
    llm.respond = [](const auto&) { ChatResponse r; r.text = "fallback answer"; return r; };
    saved = {imageMessage()};
    runToolLoop(llm, registry, {}, options, [&](const Msg& m) { saved.push_back(m); });
    expect(saved.back().text == "fallback answer", "fallback answer must be persisted");
    llm.respond = [](const auto&) { return ChatResponse{}; };
    bool rejected = false;
    try { runToolLoop(llm, registry, {}, options); } catch (const std::runtime_error&) { rejected = true; }
    expect(rejected, "empty fallback must leave input unfinished");
}

void summaryFailureKeepsHistory() {
    TestArchive storage;
    const auto key = ConversationKey::privateChat("test");
    const auto original = completedHistory();
    storage.archive.append(key, original);
    auto llm = std::make_shared<FakeLlm>();
    llm->respond = [](const auto&) -> ChatResponse { throw std::runtime_error("summary unavailable"); };
    SummaryManager summary(800, llm);
    const auto cold = storage.archive.coldRead(key, summary, 20, 4);
    expect(cold.msgs.size() == original.size(), "summary failure must preserve archived input");
    expectValidTools(cold.msgs);
    FusionUnit unit(key, original);
    unit.reColdStart(summary);
    expect(unit.context().size() == original.size(), "rebuild summary failure must preserve input");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"cold read tool pairs", coldReadKeepsToolPairs},
        {"compression tool pairs", compressionKeepsToolPairs},
        {"incomplete history repair", interruptedHistoryIsRepaired},
        {"UTF-8 summary markers", summaryParsesUtf8Markers},
        {"unanswered image compression", interruptedImageSurvivesBuildAndCompression},
        {"tool calls preserve images", toolCallsDoNotConsumeImages},
        {"interrupted tool batch", interruptedToolBatchGetsResults},
        {"terminal and fallback completion", terminalAndFallbackCompleteInput},
        {"summary failure fallback", summaryFailureKeepsHistory},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
