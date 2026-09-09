// ============================================================================
// OpenAI /v1/embeddings 兼容适配器实现（见 llm/OpenAiEmbedding.h）
//
// 请求：{"model": "...", "input": "...", "encoding_format": "float"}
//       （显式声明 float：个别端点默认返回 base64，解析成本高）
// 响应：{"data": [{"index": 0, "embedding": [0.1, ...]}], ...}
//
// 重试分类与 OpenAiCompat 一致：网络层失败 / 408 / 429 / 5xx 可重试，
// 其余 4xx 立刻失败。embed 的调用方（记忆写入/话题匹配）负责把异常
// 降级为"跳过本次写入 / 回退精确匹配"，向量端点不可用不影响聊天主链路。
// ============================================================================

#include "providers/embedding/openai/OpenAiEmbedding.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

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

bool retryableHttp(long code) {
    return code == 408 || code == 429 || (code >= 500 && code <= 599);
}

std::string bodySnippet(const std::string& body, std::size_t max = 200) {
    return body.size() > max ? body.substr(0, max) : body;
}

// data[].embedding 按 index 升序拼接（单输入通常只有一条，宽容处理多块）
std::vector<float> collectVectors(const nlohmann::json& body) {
    if (body.contains("error"))
        throw std::runtime_error("embedding 端点返回错误: " +
                                 bodySnippet(body["error"].dump()));
    if (!body.contains("data") || !body["data"].is_array() ||
        body["data"].empty())
        throw std::runtime_error("embedding 响应缺少 data | body=" +
                                 bodySnippet(body.dump()));

    std::vector<const nlohmann::json*> blocks;
    for (const auto& item : body["data"]) blocks.push_back(&item);
    std::sort(blocks.begin(), blocks.end(),
              [](const nlohmann::json* a, const nlohmann::json* b) {
                  return a->value("index", 0) < b->value("index", 0);
              });

    std::vector<float> out;
    for (const auto* item : blocks) {
        if (!item->contains("embedding") || !(*item)["embedding"].is_array())
            throw std::runtime_error("embedding 响应缺少 embedding 数组");
        for (const auto& v : (*item)["embedding"])
            out.push_back(std::isfinite(v.get<double>())
                              ? static_cast<float>(v.get<double>())
                              : 0.0f);
    }
    return out;
}

} // namespace

OpenAiEmbedding::OpenAiEmbedding(EmbeddingConfig cfg) : cfg_(std::move(cfg)) {}

std::string OpenAiEmbedding::resolveEndpoint(const std::string& baseUrl) {
    std::string base = trimSlash(baseUrl);
    if (endsWith(base, "/embeddings")) return base;
    if (endsWith(base, "/v1")) return base + "/embeddings";
    return base + "/v1/embeddings";
}

std::vector<float> OpenAiEmbedding::embed(const std::string& text) const {
    const std::string endpoint = resolveEndpoint(cfg_.baseUrl);
    nlohmann::json payload{{"model", cfg_.model},
                           {"input", text},
                           {"encoding_format", "float"}};

    const int maxAttempts = cfg_.maxRetries + 1;
    long lastStatus = 0;
    std::string lastBody;
    cpr::Error lastErr{};

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (attempt > 0) {
            const double delayMs =
                std::min<double>(cfg_.retryInitialDelayMs *
                                     std::pow(cfg_.retryBackoffFactor, attempt - 1),
                                 10'000.0);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<long long>(delayMs)));
        }

        const cpr::Response r = cpr::Post(
            cpr::Url{endpoint},
            cpr::Header{{"Content-Type", "application/json"},
                        {"Authorization", "Bearer " + cfg_.apiKey}},
            cpr::Body{payload.dump()},
            cpr::Timeout{std::chrono::milliseconds(cfg_.httpTimeoutMs)},
            cpr::ConnectTimeout{std::chrono::milliseconds(10'000)});

        if (r.error.code == cpr::ErrorCode::OK && r.status_code >= 200 &&
            r.status_code < 300) {
            nlohmann::json body;
            try {
                body = nlohmann::json::parse(r.text);
            } catch (const std::exception& e) {
                throw std::runtime_error(std::string("embedding 响应不是合法 JSON: ") +
                                         e.what());
            }
            std::vector<float> vec = collectVectors(body);
            if (vec.empty())
                throw std::runtime_error("embedding 响应为空向量");
            return vec;
        }

        lastStatus = r.status_code;
        lastBody = r.text;
        lastErr = r.error;

        const bool transportFailed = r.error.code != cpr::ErrorCode::OK;
        if (transportFailed || retryableHttp(r.status_code)) continue;
        break;  // 4xx 业务错误：重试无意义
    }

    std::ostringstream msg;
    msg << "embedding 请求失败 ";
    if (lastErr.code != cpr::ErrorCode::OK)
        msg << "(传输层: " << lastErr.message << ")";
    else
        msg << "(HTTP " << lastStatus << ")";
    msg << ": " << bodySnippet(lastBody);
    throw std::runtime_error(msg.str());
}

} // namespace mio
