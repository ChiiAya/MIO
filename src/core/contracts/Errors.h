#pragma once
// ============================================================================
// 稳定错误码 + 工具返回 envelope（共享，冻结）
//
// 工具返回统一为：{"ok": bool, "data": {...}, "error": {"code","message"}}
// 错误码是协议的一部分，模型与测试都依赖它们，不得随意改名。
//
// 人工审阅相关约定：
//   * HUMAN_REVIEW_REQUIRED 是【错误码】（当前版本任何批准调用都必须返回它）；
//   * HUMAN_REVIEW_PENDING  是【审阅状态】（提案持久化后的默认状态）；
//   * HUMAN_REVIEW_ACTOR / REASON / TIMESTAMP 仅为待接入字段，
//     当前版本 actor 必须为空、reviewedAt 必须为 0，子模块不得伪造。
// ============================================================================

#include <string>

#include <nlohmann/json.hpp>

namespace mio {

// 人工审阅占位符（协议的一部分，勿改字面量）
inline const char* const kHumanReviewRequired = "HUMAN_REVIEW_REQUIRED";
inline const char* const kHumanReviewPending = "HUMAN_REVIEW_PENDING";
inline const char* const kHumanReviewApproved = "HUMAN_REVIEW_APPROVED";
inline const char* const kHumanReviewRejected = "HUMAN_REVIEW_REJECTED";
inline const char* const kHumanReviewActorTag = "HUMAN_REVIEW_ACTOR";
inline const char* const kHumanReviewReasonTag = "HUMAN_REVIEW_REASON";
inline const char* const kHumanReviewTimestampTag = "HUMAN_REVIEW_TIMESTAMP";

// 摘要重试耗尽：与人工事实审阅区分，单独错误码
inline const char* const kSummaryRetryExhausted = "SUMMARY_RETRY_EXHAUSTED";
// Hindsight 真实接入尚未实现（配置/部署/联网/SDK 均属此占位）
inline const char* const kHindsightAdapterTodo = "HINDSIGHT_ADAPTER_TODO";

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,        // INVALID_ARGUMENT
    LimitExceeded,          // LIMIT_EXCEEDED
    NotFoundOrForbidden,    // NOT_FOUND_OR_FORBIDDEN（不可见与不存在统一此码）
    HumanReviewRequired,    // HUMAN_REVIEW_REQUIRED
    StorageUnavailable,     // STORAGE_UNAVAILABLE
    Conflict,               // CONFLICT
    SummaryRetryExhausted,  // SUMMARY_RETRY_EXHAUSTED
    HindsightAdapterTodo,   // HINDSIGHT_ADAPTER_TODO
};

inline const char* toString(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "INVALID_ARGUMENT";
    case ErrorCode::LimitExceeded: return "LIMIT_EXCEEDED";
    case ErrorCode::NotFoundOrForbidden: return "NOT_FOUND_OR_FORBIDDEN";
    case ErrorCode::HumanReviewRequired: return kHumanReviewRequired;
    case ErrorCode::StorageUnavailable: return "STORAGE_UNAVAILABLE";
    case ErrorCode::Conflict: return "CONFLICT";
    case ErrorCode::SummaryRetryExhausted: return kSummaryRetryExhausted;
    case ErrorCode::HindsightAdapterTodo: return kHindsightAdapterTodo;
    }
    return "INVALID_ARGUMENT";
}

// 统一返回 envelope：{ok, data, error}
struct ToolResult {
    bool ok = false;
    nlohmann::json data = nlohmann::json::object();
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    static ToolResult success(nlohmann::json payload) {
        ToolResult r;
        r.ok = true;
        r.data = std::move(payload);
        return r;
    }
    static ToolResult failure(ErrorCode c, std::string msg) {
        ToolResult r;
        r.ok = false;
        r.code = c;
        r.message = std::move(msg);
        return r;
    }

    // 序列化为字符串（工具 handler 的返回值）
    std::string toJsonString() const {
        nlohmann::json j;
        j["ok"] = ok;
        if (ok) {
            j["data"] = data;
            j["error"] = nullptr;
        } else {
            j["data"] = nullptr;
            j["error"] = {{"code", toString(code)}, {"message", message}};
        }
        return j.dump();
    }
};

// 审阅占位（当前版本 actor 必须为空、reviewedAt 必须为 0）
struct ReviewPlaceholder {
    std::string status;   // HUMAN_REVIEW_* 或空
    std::string actor;    // 当前版本必须为空
    std::string reason;   // 当前版本可为空
    std::int64_t reviewedAt = 0;  // 当前版本必须为 0

    bool actorIsFabricated() const { return !actor.empty() || reviewedAt != 0; }
};

// 审阅服务返回
struct ReviewResult {
    ErrorCode code = ErrorCode::HumanReviewRequired;
    std::string status;    // HUMAN_REVIEW_* 或空
    std::string message;
    bool ok() const { return code == ErrorCode::Ok; }
};

} // namespace mio
