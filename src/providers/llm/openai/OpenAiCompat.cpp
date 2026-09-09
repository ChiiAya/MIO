// ============================================================================
// OpenAI 兼容适配器实现
//
// 实现按函数拆解：
//   1. resolveChatEndpoint  —— baseUrl 宽容归一化
//   2. toWireMessages       —— 内部 Msg -> OpenAI 消息方言（唯一出口）
//   3. buildPayload         —— 请求体组装（模型参数、工具注入、厂商怪癖修正）
//   4. chat 的重试循环       —— 哪些错误值得重试
//   5. parseResponse        —— 响应解析 + Usage 记账（cached 单独记！）
//
// cpr 用法备忘：
//   cpr::Post(Url, Headers, Body, Timeout...) 同步阻塞，返回 Response{
//     .status_code, .text, .error }；
//   网络层失败时 status_code 为 0 且 error.code != OK —— 必须同时判断两者。
// ============================================================================

#include "providers/llm/openai/OpenAiCompat.h"

#include <cpr/cpr.h>

#include "context/costEstimator/Tokens.h"
#include "log/Log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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

// --- baseUrl 归一化 -------------------------------------------------------
// OpenAiConfig 约定：baseUrl 只填到 /v1 之前（用户最容易填错的地方）。
// 规则：已带 /chat/completions → 原样；以 /v1 结尾 → 补路径；
//      其他 → 一律补 /v1/chat/completions。
std::string resolveChatEndpoint(const std::string& baseUrl) {
    std::string base = trimSlash(baseUrl);
    if (endsWith(base, "/chat/completions")) return base;
    if (endsWith(base, "/v1")) return base + "/chat/completions";
    return base + "/chat/completions";
}

// --- wire 格式翻译 ---------------------------------------------------------
// 将内部 Msg 转换为 OpenAI 协议的 messages 数组（唯一出口）

// 用户消息来源标签：[平台][private]（私聊）或 [平台][group:群号]（群聊），
// 再接 [sender:人]。"人" = Router 注入的昵称（senderName），未命中回退原始
// senderId；让模型知道"这句话从哪个平台、哪个会话、谁"说的；无身份
// （旧历史/系统内部请求）则不打。
std::string senderTag(const Msg& m) {
    if (m.platform.empty()) return "";
    const std::string& who =
        !m.senderName.empty() ? m.senderName : m.senderId;
    if (who.empty()) return "";
    const std::string conv =
        m.groupId.empty() ? "private" : "group:" + m.groupId;
    return "[" + m.platform + "][" + conv + "][sender:" + who + "] ";
}

nlohmann::json toWireMessages(const ChatRequest& req) {
    auto wire = nlohmann::json::array();

    // system 放最前。内容必须逐字稳定 —— 它是 prompt cache 前缀的头部，
    // 每次变动 = 整段历史重新计价。
    if (!req.systemPrompt.empty())
        wire.push_back({{"role", "system"}, {"content", req.systemPrompt}});

    for (const auto& m : req.messages) {
        nlohmann::json j{{"role", nullptr}};
        switch (m.role) {
        case Role::System:
            j["role"] = "system";
            j["content"] = m.text; // 内部 history 不存 system，这里只是兜底
            break;

        case Role::User: {
            j["role"] = "user";
            const std::string tag = senderTag(m);
            if (m.isTextOnly()) {
                j["content"] = tag + m.text;
            } else {
                auto parts = nlohmann::json::array();
                bool tagged = false;
                for (const auto& p : m.parts) {
                    if (p.kind == Part::Kind::Image) {
                        parts.push_back({
                            {"type", "image_url"},
                            {"image_url", {{"url", p.text}}},
                        });
                    } else {
                        // 来源标签附在首个文本段前
                        parts.push_back(
                            {{"type", "text"},
                             {"text", tagged ? p.text : tag + p.text}});
                        tagged = true;
                    }
                }
                j["content"] = std::move(parts);
            }
            break;
        }

        case Role::Assistant: {
            j["role"] = "assistant";
            j["content"] = m.text.empty() ? nlohmann::json(nullptr) : nlohmann::json(m.text);
            // DeepSeek / MiMo 等推理系要求带 tool_calls 的历史 assistant 消息
            // 原样回传 reasoning_content；这里统一渲染，非空才带上。
            if (!m.reasoningContent.empty())
                j["reasoning_content"] = m.reasoningContent;
            if (!m.toolCalls.empty()) {
                auto calls = nlohmann::json::array();
                for (const auto& tc : m.toolCalls) {
                    calls.push_back({{"id", tc.id},
                                     {"type", "function"},
                                     {"function",
                                      {{"name", tc.name},
                                       {"arguments", tc.argumentsJson}}}});
                }
                j["tool_calls"] = std::move(calls);
            }
            break;
        }

        case Role::Tool:
            // 工具结果必须是纯字符串 content；tool_call_id 回指调用
            j["role"] = "tool";
            j["tool_call_id"] = m.toolCallId;
            j["content"] = m.text;
            break;
        }
        wire.push_back(std::move(j));
    }
    return wire;
}

// --- 重试分类表 -------------------------------------------------------------
// 可重试：网络层失败、408 超时、429 限流、所有 5xx。
// 不可重试：其余 4xx —— payload/鉴权错误重试无意义。
bool retryableHttp(long code) {
    return code == 408 || code == 429 || (code >= 500 && code <= 599);
}

std::string bodySnippet(const std::string& body, std::size_t max = 400) {
    return cutUtf8(body, max); // cutUtf8 见 context/costEstimator/Tokens.h，防中文截半截变乱码
}

std::string toString(ReasoningEffort e) {
    switch (e) {
    case ReasoningEffort::Low: return "low";
    case ReasoningEffort::Medium: return "medium";
    case ReasoningEffort::High: return "high";
    case ReasoningEffort::XHigh: return "xhigh";
    case ReasoningEffort::Max: return "max";
    }
    throw std::runtime_error("invalid reasoning effort");
}

std::string toString(Thinking t) {
    switch (t) {
    case Thinking::Enabled: return "enabled";
    case Thinking::Disabled: return "disabled";
    }
    throw std::runtime_error("invalid thinking");
}

} // namespace

OpenAiCompat::OpenAiCompat(OpenAiConfig cfg) : cfg_(std::move(cfg)) {}

void OpenAiCompat::normalizeQuirks(nlohmann::json& p) const {
    //  * DeepSeek-V4 / MiMo 推理系：assistant 历史必须带 reasoning_content 字段
    //  * Gemini 原生 API：tool 结果要包成 JSON 对象
    //  * 部分端点拒绝 content 为 null 的 assistant 消息 → 可在此替换为 ""
    const bool isReasoningFamily =
        cfg_.baseUrl == "https://api.xiaomimimo.com/v1/chat/completions" ||
        cfg_.baseUrl == "https://api.deepseek.com/chat/completions";
    if (isReasoningFamily && p.contains("tools")) {
        // reasoning_content 已在 toWireMessages() 中随每条 assistant 消息原样写入，
        // 这里只保留确认日志，便于排查 DeepSeek/MiMo 的回传链路。
        log::debug("OpenAiCompat::normalizeQuirks", "已随历史消息回传 reasoning_content");
    } else {
        log::debug("OpenAiCompat::normalizeQuirks", cfg_.baseUrl + "非特殊处理对象");
    }
}

ChatResponse OpenAiCompat::chat(const ChatRequest& req) {
    const std::string endpoint = resolveChatEndpoint(cfg_.baseUrl);
    nlohmann::json payload = buildPayload(req);

    const int maxAttempts = cfg_.maxRetries + 1;
    long lastStatus = 0;
    std::string lastBody;
    cpr::Error lastErr{};

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (attempt > 0) {
            // 指数退避 + 上限保护；真实限流场景下首退避要给足（>=1s）
            double delayMs =
                std::min<double>(cfg_.retryInitialDelayMs *
                                     std::pow(cfg_.retryBackoffFactor, attempt - 1),
                                 30'000.0);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<long long>(delayMs)));
            payload = buildPayload(req); // 重建一次
        }

        cpr::Response r = cpr::Post(
            cpr::Url{endpoint},
            cpr::Header{{"Content-Type", "application/json"},
                        {"Authorization", "Bearer " + cfg_.apiKey}},
            cpr::Body{payload.dump()},
            cpr::Timeout{std::chrono::milliseconds(cfg_.httpTimeoutMs)},
            cpr::ConnectTimeout{std::chrono::milliseconds(10'000)});
        log::debug("OpenAiCompat", "POST " + payload.dump());

        if (r.error.code == cpr::ErrorCode::OK && r.status_code >= 200 &&
            r.status_code < 300) {
            log::debug("OpenAiCompat",
                       "响应(" + std::to_string(r.status_code) + ") " + r.text);
            return parseResponse(r.text);
        }

        lastStatus = r.status_code;
        lastBody = r.text;
        lastErr = r.error;

        const bool transportFailed = r.error.code != cpr::ErrorCode::OK;
        if (transportFailed || retryableHttp(r.status_code)) {
            const std::string why =
                transportFailed ? r.error.message
                                : "HTTP " + std::to_string(r.status_code);
            log::warn("OpenAiCompat", "请求失败(" + why +
                          ")，将进行第 " + std::to_string(attempt + 2) + " 次尝试");
            continue; // 再试
        }
        break; // 4xx 业务错误：立刻失败，把服务端的话带给调用者
    }

    std::ostringstream msg;
    msg << "LLM 请求失败 ";
    if (lastErr.code != cpr::ErrorCode::OK)
        msg << "(传输层: " << lastErr.message << ")";
    else
        msg << "(HTTP " << lastStatus << ")";
    msg << ": " << bodySnippet(lastBody);
    throw std::runtime_error(msg.str());
}

nlohmann::json OpenAiCompat::buildPayload(const ChatRequest& req) const {
    nlohmann::json p;
    p["model"] = !req.modelOverride.empty() ? req.modelOverride : cfg_.model;
    p["messages"] = toWireMessages(req);

    if (!req.tools.empty()) {
        auto tools = nlohmann::json::array();
        auto defs = req.tools;
        std::sort(defs.begin(), defs.end(),
                  [](const ToolDef& a, const ToolDef& b) { return a.name < b.name; });
        for (const auto& d : defs) {
            tools.push_back({{"type", "function"},
                             {"function",
                              {{"name", d.name},
                               {"description", d.description},
                               {"parameters", d.parametersJsonSchema}}}});
        }
        p["tools"] = std::move(tools);
    }

    if (cfg_.reasoningEffort) p["reasoning_effort"] = toString(*cfg_.reasoningEffort);
    if (cfg_.thinking) p["thinking"] = toString(*cfg_.thinking);
    if (cfg_.topP) p["top_p"] = *cfg_.topP;
    if (cfg_.temperature) p["temperature"] = *cfg_.temperature;
    if (cfg_.maxTokens) p["max_tokens"] = *cfg_.maxTokens;

    normalizeQuirks(p);
    return p;
}

ChatResponse OpenAiCompat::parseResponse(const std::string& bodyText) {
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(bodyText);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("LLM 响应不是合法 JSON: ") + e.what() +
                                 " | body=" + bodySnippet(bodyText, 200));
    }

    if (body.contains("error"))
        throw std::runtime_error("LLM 返回错误: " +
                                 bodySnippet(body["error"].dump()));

    if (!body.contains("choices") || body["choices"].empty())
        throw std::runtime_error("LLM 响应缺少 choices | body=" +
                                 bodySnippet(bodyText));

    ChatResponse out;
    const auto& choice = body["choices"][0];
    const auto& message = choice.value("message", nlohmann::json::object());

    if (auto it = message.find("content"); it != message.end() && it->is_string())
        out.text = it->get<std::string>();
    if (auto it = message.find("reasoning_content"); it != message.end() && it->is_string())
        out.reasoning = it->get<std::string>();

    if (auto it = message.find("tool_calls"); it != message.end() && it->is_array()) {
        for (const auto& tc : *it) {
            ToolCall call;
            call.id = tc.value("id", "");
            call.name = tc["function"].value("name", "");
            // arguments 是 JSON 字符串；个别实现直接给对象，宽容处理两种情况
            if (auto a = tc["function"].find("arguments");
                a != tc["function"].end() && a->is_string()) {
                call.argumentsJson = a->get<std::string>();
            } else if (a != tc["function"].end()) {
                call.argumentsJson = a->dump();
            } else {
                call.argumentsJson = "{}";
            }
            out.toolCalls.push_back(std::move(call));
        }
    }

    out.finishReason = choice.value("finish_reason", "");

    // --- Usage 记账（inputCached 单独记，低价供应商普遍支持前缀缓存）---
    if (auto u = body.find("usage"); u != body.end()) {
        out.usage.inputOther = u->value("prompt_tokens", 0);
        out.usage.output = u->value("completion_tokens", 0);
        if (auto det = u->find("prompt_tokens_details"); det != u->end() && det->is_object()) {
            out.usage.inputCached = det->value("cached_tokens", 0);
            // 防御供应商侧脏数据：cached 理论上是 prompt 的子集（OpenAI/DeepSeek
            // 风格），超了就钳住 —— 不然像本次联调一样会记出负数账
            out.usage.inputCached =
                std::min(out.usage.inputCached, out.usage.inputOther);
            // 注意 cached_tokens 已含在 prompt_tokens 里，不重复计数
        }
    }

    out.raw = std::move(body);
    return out;
}

} // namespace mio
