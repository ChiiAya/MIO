#include "core/contracts/LifecycleConfig.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>

namespace mio {
namespace {

std::optional<std::string> env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

// 严格整数解析：整串必须是十进制整数（允许前导 +/- 与空白），否则失败
bool parseStrictInt(const std::string& s, long long& out) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    if (i >= j) return false;
    std::string body = s.substr(i, j - i);
    std::size_t k = 0;
    if (body[k] == '+' || body[k] == '-') ++k;
    if (k >= body.size()) return false;
    for (std::size_t m = k; m < body.size(); ++m) {
        if (!std::isdigit(static_cast<unsigned char>(body[m]))) return false;
    }
    try {
        out = std::stoll(body);
    } catch (...) {
        return false;
    }
    return true;
}

bool parseStrictBool(const std::string& s, bool& out) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
        out = true;
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

ConfigValidationResult validateConversationLifecycle(const ConversationLifecycleConfig& cfg) {
    ConfigValidationResult r;
    if (cfg.silenceTimeoutSeconds < 1 || cfg.silenceTimeoutSeconds > 86400) {
        r.error = "conversation.silenceTimeoutSeconds 必须在 1..86400 之间，实际 " +
                  std::to_string(cfg.silenceTimeoutSeconds);
        return r;
    }
    if (cfg.minMessagesBeforeSummary < 0 || cfg.minMessagesBeforeSummary > 1000) {
        r.error = "conversation.minMessagesBeforeSummary 必须在 0..1000 之间，实际 " +
                  std::to_string(cfg.minMessagesBeforeSummary);
        return r;
    }
    if (cfg.maxPendingSummaryRetries < 0 || cfg.maxPendingSummaryRetries > 10) {
        r.error = "conversation.maxPendingSummaryRetries 必须在 0..10 之间，实际 " +
                  std::to_string(cfg.maxPendingSummaryRetries);
        return r;
    }
    r.ok = true;
    return r;
}

nlohmann::json toJson(const ConversationLifecycleConfig& cfg) {
    return nlohmann::json{
        {"silenceTimeoutSeconds", cfg.silenceTimeoutSeconds},
        {"summarizeOnSilence", cfg.summarizeOnSilence},
        {"minMessagesBeforeSummary", cfg.minMessagesBeforeSummary},
        {"maxPendingSummaryRetries", cfg.maxPendingSummaryRetries},
    };
}

ConversationLifecycleConfig conversationLifecycleFromJson(const nlohmann::json& j) {
    ConversationLifecycleConfig cfg;  // 缺失字段保持默认值
    applyConversationLifecycleJson(j, cfg);
    return cfg;
}

void applyConversationLifecycleJson(const nlohmann::json& j,
                                    ConversationLifecycleConfig& cfg) {
    if (!j.is_object()) {
        throw std::runtime_error("conversation 配置节必须是对象");
    }
    if (j.contains("silenceTimeoutSeconds") && !j["silenceTimeoutSeconds"].is_null()) {
        if (!j["silenceTimeoutSeconds"].is_number_integer())
            throw std::runtime_error("conversation.silenceTimeoutSeconds 必须是整数");
        cfg.silenceTimeoutSeconds = j["silenceTimeoutSeconds"].get<int>();
    }
    if (j.contains("summarizeOnSilence") && !j["summarizeOnSilence"].is_null()) {
        if (!j["summarizeOnSilence"].is_boolean())
            throw std::runtime_error("conversation.summarizeOnSilence 必须是布尔值");
        cfg.summarizeOnSilence = j["summarizeOnSilence"].get<bool>();
    }
    if (j.contains("minMessagesBeforeSummary") && !j["minMessagesBeforeSummary"].is_null()) {
        if (!j["minMessagesBeforeSummary"].is_number_integer())
            throw std::runtime_error("conversation.minMessagesBeforeSummary 必须是整数");
        cfg.minMessagesBeforeSummary = j["minMessagesBeforeSummary"].get<int>();
    }
    if (j.contains("maxPendingSummaryRetries") && !j["maxPendingSummaryRetries"].is_null()) {
        if (!j["maxPendingSummaryRetries"].is_number_integer())
            throw std::runtime_error("conversation.maxPendingSummaryRetries 必须是整数");
        cfg.maxPendingSummaryRetries = j["maxPendingSummaryRetries"].get<int>();
    }
    // 校验作用于合并后的候选值：非法时整次重载失败（不得部分套用）
    const auto v = validateConversationLifecycle(cfg);
    if (!v.ok) throw std::runtime_error(v.error);
}

bool applyConversationLifecycleEnv(ConversationLifecycleConfig& cfg, std::string* error) {
    if (auto v = env("MIO_SILENCE_TIMEOUT_SECONDS")) {
        long long parsed = 0;
        if (!parseStrictInt(*v, parsed)) {
            if (error) *error = "MIO_SILENCE_TIMEOUT_SECONDS 不是合法整数: " + *v;
            return false;
        }
        cfg.silenceTimeoutSeconds = static_cast<int>(parsed);
    }
    if (auto v = env("MIO_SUMMARIZE_ON_SILENCE")) {
        bool parsed = true;
        if (!parseStrictBool(*v, parsed)) {
            if (error) *error = "MIO_SUMMARIZE_ON_SILENCE 不是合法布尔值: " + *v;
            return false;
        }
        cfg.summarizeOnSilence = parsed;
    }
    if (auto v = env("MIO_MIN_MESSAGES_BEFORE_SUMMARY")) {
        long long parsed = 0;
        if (!parseStrictInt(*v, parsed)) {
            if (error) *error = "MIO_MIN_MESSAGES_BEFORE_SUMMARY 不是合法整数: " + *v;
            return false;
        }
        cfg.minMessagesBeforeSummary = static_cast<int>(parsed);
    }
    if (auto v = env("MIO_MAX_PENDING_SUMMARY_RETRIES")) {
        long long parsed = 0;
        if (!parseStrictInt(*v, parsed)) {
            if (error) *error = "MIO_MAX_PENDING_SUMMARY_RETRIES 不是合法整数: " + *v;
            return false;
        }
        cfg.maxPendingSummaryRetries = static_cast<int>(parsed);
    }
    const auto validation = validateConversationLifecycle(cfg);
    if (!validation.ok) {
        if (error) *error = validation.error;
        return false;
    }
    return true;
}

} // namespace mio
