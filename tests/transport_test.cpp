#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include "providers/llm/openai/OpenAiCompat.h"

using namespace mio;
int main() {
    ix::initNetSystem();
    std::unique_ptr<ix::HttpServer> server;
    const int firstPort = 40000 + static_cast<int>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 15000);
    int port = firstPort;
    for (int i = 0; i < 32; ++i, ++port) {
        auto candidate = std::make_unique<ix::HttpServer>(port, "127.0.0.1");
        if (candidate->listen().first) { server = std::move(candidate); break; }
    }
    if (!server) throw std::runtime_error("unable to bind local mock LLM");
    std::mutex mutex;
    std::vector<nlohmann::json> payloads;
    server->setOnConnectionCallback([&](const ix::HttpRequestPtr& request, const auto&) {
        std::lock_guard<std::mutex> lock(mutex);
        payloads.push_back(nlohmann::json::parse(request->body));
        if (payloads.size() == 1)
            return std::make_shared<ix::HttpResponse>(429, "Too Many Requests", ix::HttpErrorCode::Ok,
                ix::WebSocketHttpHeaders{{"Content-Type", "application/json"}}, "{\"error\":\"retry\"}");
        return std::make_shared<ix::HttpResponse>(200, "OK", ix::HttpErrorCode::Ok,
            ix::WebSocketHttpHeaders{{"Content-Type", "application/json"}},
            R"({"choices":[{"finish_reason":"stop","message":{"content":"image received"}}]})");
    });
    server->start();
    OpenAiConfig cfg;
    cfg.baseUrl = "http://127.0.0.1:" + std::to_string(port);
    cfg.model = "mock"; cfg.maxRetries = 1; cfg.retryInitialDelayMs = 0; cfg.httpTimeoutMs = 3000;
    OpenAiCompat llm(cfg);
    ChatRequest request;
    Msg msg{Role::User};
    msg.text = "describe"; msg.platform = "qq"; msg.groupId = "20000";
    msg.senderId = "10000"; msg.messageId = "55"; msg.replyToMessageId = "54";
    msg.parts.push_back({Part::Kind::Image, "data:image/png;base64,aGVsbG8=", true});
    request.messages.push_back(msg);
    const auto response = llm.chat(request);
    server->stop();
    if (response.text != "image received" || payloads.size() != 2 || payloads[0] != payloads[1])
        throw std::runtime_error("transient HTTP retry changed multimedia payload");
    const auto& content = payloads[1]["messages"][0]["content"];
    const auto text = content[0]["text"].get<std::string>();
    if (content[1]["type"] != "image_url" || text.find("[message_id:55]") == std::string::npos ||
        text.find("[reply_to_message_id:54]") == std::string::npos)
        throw std::runtime_error("wire request lost image or quote metadata");
    ix::uninitNetSystem();
    std::cout << "HTTP retry and wire metadata tests passed\n";
}
