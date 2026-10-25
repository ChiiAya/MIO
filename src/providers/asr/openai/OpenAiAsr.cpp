#include "providers/asr/openai/OpenAiAsr.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace mio {

namespace {

std::string trimSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string bodySnippet(const std::string& body, std::size_t max = 200) {
    return body.size() > max ? body.substr(0, max) : body;
}

} // namespace

OpenAiAsr::OpenAiAsr(AsrConfig cfg) : cfg_(std::move(cfg)) {}

std::string OpenAiAsr::resolveEndpoint(const std::string& baseUrl) {
    std::string base = trimSlash(baseUrl);
    if (endsWith(base, "/audio/transcriptions")) return base;
    if (endsWith(base, "/v1")) return base + "/audio/transcriptions";
    return base + "/v1/audio/transcriptions";
}

std::string OpenAiAsr::transcribeFile(const std::string& audioPath) const {
    if (audioPath.empty()) throw std::runtime_error("ASR 音频路径为空");
    if (cfg_.baseUrl.empty()) throw std::runtime_error("ASR baseUrl 未配置");

    std::vector<cpr::Part> parts;
    parts.emplace_back("file", cpr::Files{cpr::File{audioPath}});
    parts.emplace_back("model", cfg_.model);
    if (!cfg_.language.empty()) parts.emplace_back("language", cfg_.language);

    const cpr::Response r = cpr::Post(
        cpr::Url{resolveEndpoint(cfg_.baseUrl)},
        cpr::Header{{"Authorization", "Bearer " + cfg_.apiKey}},
        cpr::Multipart{std::move(parts)},
        cpr::Timeout{std::chrono::milliseconds(cfg_.httpTimeoutMs)},
        cpr::ConnectTimeout{std::chrono::milliseconds(10'000)});

    if (r.error.code != cpr::ErrorCode::OK) {
        throw std::runtime_error("ASR 请求失败(传输层): " + r.error.message);
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        throw std::runtime_error("ASR 请求失败(HTTP " +
                                 std::to_string(r.status_code) + "): " +
                                 bodySnippet(r.text));
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(r.text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("ASR 响应不是合法 JSON: ") +
                                 e.what());
    }
    if (body.contains("error")) {
        throw std::runtime_error("ASR 端点返回错误: " +
                                 bodySnippet(body["error"].dump()));
    }

    // 主流端点返回 {"text": "..."}；部分厂商包一层 result/transcription
    for (const char* key : {"text", "result", "transcription"}) {
        if (body.contains(key) && body[key].is_string()) {
            return body[key].get<std::string>();
        }
    }
    throw std::runtime_error("ASR 响应缺少 text 字段: " +
                             bodySnippet(body.dump()));
}

} // namespace mio
