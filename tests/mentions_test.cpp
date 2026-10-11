#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>

#include "adapters/napcat/OneBotMessage.h"
#include "adapters/napcat/OneBotParser.h"
#include "context/conversationFusion/FusionRouter.h"

using namespace mio;

namespace {

nlohmann::json eventWith(const nlohmann::json& message) {
    return {{"post_type", "message"}, {"message_type", "group"},
            {"self_id", "90000"}, {"user_id", "10000"},
            {"group_id", "20000"}, {"message_id", "-123"},
            {"sender", {{"nickname", "Sender"}}}, {"message", message}};
}

void parseArrayAndCq() {
    const auto array = nlohmann::json::array({
        {{"type", "reply"}, {"data", {{"id", "-456"}}}},
        {{"type", "at"}, {"data", {{"qq", 12345}}}},
        {{"type", "text"}, {"data", {{"text", "6789 says hi"}}}}
    });
    const auto parsedArray = napcat::eventToIncomingMessage(
        napcat::parseOneBotEvent(eventWith(array)));
    assert(parsedArray);
    assert(parsedArray->messageId == "-123");
    assert(parsedArray->replyToMessageId == "-456");
    assert(parsedArray->atUserIds == std::vector<std::string>{"12345"});
    assert(parsedArray->text.find("@12345 6789") != std::string::npos);

    const auto parsedCq = napcat::eventToIncomingMessage(napcat::parseOneBotEvent(
        eventWith("[CQ:reply,id=-456][CQ:at,qq=12345]6789 says hi")));
    assert(parsedCq);
    assert(parsedCq->text == parsedArray->text);
    assert(parsedCq->atUserIds == parsedArray->atUserIds);
    assert(parsedCq->replyToMessageId == parsedArray->replyToMessageId);

    auto rawOnly = eventWith(nlohmann::json::array());
    rawOnly["raw_message"] = "[CQ:at,qq=22222]hi";
    const auto fallback = napcat::eventToIncomingMessage(napcat::parseOneBotEvent(rawOnly));
    assert(fallback && fallback->atUserIds == std::vector<std::string>{"22222"});
}

void preserveEscapingAndMedia() {
    const auto escaped = napcat::parseOneBotEvent(eventWith(
        "&#91;CQ:at,qq=99999&#93; &amp;#91; &amp;amp; "
        "[CQ:image,file=a&#44;b,url=https://example.test/a?x=1&amp;y=2]"));
    assert(escaped.text.find("[CQ:at,qq=99999] &#91; &amp;") != std::string::npos);
    const auto incoming = napcat::eventToIncomingMessage(escaped);
    assert(incoming && incoming->atUserIds.empty());
    assert(escaped.attachments.size() == 1);
    assert(escaped.attachments[0].fileName == "a,b");
    assert(escaped.attachments[0].url == "https://example.test/a?x=1&y=2");

    const auto literal = napcat::parseOneBotEvent(eventWith(nlohmann::json::array({
        {{"type", "text"}, {"data", {{"text", "&#91;CQ:at,qq=99999&#93;"}}}}
    })));
    assert(literal.text == "&#91;CQ:at,qq=99999&#93;");
    const auto malformed = napcat::parseOneBotEvent(eventWith("hello [CQ:at,qq=12345"));
    assert(malformed.text == "hello [CQ:at,qq=12345");
}

void serializeOutgoing() {
    OutgoingMessage out;
    out.text = "literal [CQ:at,qq=55555]";
    out.mentionUserIds = {"12345", "67890"};
    out.replyToMessageId = "-456";
    const auto group = napcat::buildSendParams(ConversationKey::groupChat("20000"), out);
    assert(group.at("message_type") == "group");
    assert(group.at("group_id") == 20000);
    const auto& segments = group.at("message");
    assert(segments.at(0).at("type") == "reply");
    assert(segments.at(0).at("data").at("id") == "-456");
    assert(segments.at(1).at("type") == "at");
    assert(segments.at(1).at("data").at("qq") == "12345");
    assert(segments.at(2).at("data").at("qq") == "67890");
    assert(segments.back().at("type") == "text");
    assert(segments.back().at("data").at("text") == out.text);

    const auto direct = napcat::buildSendParams(ConversationKey::privateChat("33333"), out);
    assert(direct.at("message_type") == "private");
    assert(direct.at("user_id") == 33333);
    assert(!direct.contains("group_id"));
}

void mapMentions(const std::filesystem::path& temp) {
    RelationshipGraph graph(temp / "relationships.json");
    graph.ensureMio("Mio");
    const auto seen = graph.onSeen("qq", "12345", "Alice", 1000);
    Achieve achieve(temp / "history");
    FusionRouter router(graph, achieve, nullptr);

    IncomingMessage msg;
    msg.conversation = ConversationKey::groupChat("20000");
    msg.platform = "qq";
    msg.groupId = "20000";
    msg.senderId = "10000";
    msg.text = "@12345 hi; mail@54321.com @65432suffix @abc (@77777) @88888你好";
    msg.atUserIds = {"12345"};
    router.route(msg, 1001);
    assert(graph.findByPlatform("qq", "77777"));
    assert(graph.findByPlatform("qq", "88888"));
    assert(!graph.findByPlatform("qq", "54321"));
    assert(!graph.findByPlatform("qq", "65432"));
    assert(!graph.findByPlatform("qq", "65432suffix"));
    assert(!graph.findByPlatform("qq", "abc"));
    const auto context = router.mentionContext({"12345", "12345", "all"}, "qq");
    assert(context.find("12345") != std::string::npos);
    assert(context.find("Alice") != std::string::npos);
    assert(context.find(seen.internalId) != std::string::npos);
    assert(context.find("12345", context.find("12345") + 5) == std::string::npos);
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("mio-mentions-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    parseArrayAndCq();
    preserveEscapingAndMedia();
    serializeOutgoing();
    mapMentions(root);
    std::filesystem::remove_all(root);
    std::cout << "mention and OneBot regression tests passed\n";
}
