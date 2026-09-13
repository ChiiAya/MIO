#pragma once
// ============================================================================
// ConversationLifecycleConfig（共享契约，冻结）—— 会话生命周期配置节
//
// 独立于 InputBufferConfig：「对话结束」不混入输入防抖配置。
//   * InputBufferConfig::debounceMs      只负责合并短时连续消息；
//   * conversation.silenceTimeoutSeconds 负责判断一轮对话结束。
//
// 校验规则（必须整体失败，不得部分套用）：
//   * silenceTimeoutSeconds      1..86400
//   * minMessagesBeforeSummary   0..1000（0 表示不额外限制，但空范围仍不能总结）
//   * maxPendingSummaryRetries   0..10（不含首次尝试；0 = 首次失败后不自动重试）
//   * 缺少配置项使用默认值；类型错误或越界时整次配置重载失败。
//
// 私聊和群聊暂时使用同一阈值；按会话类型覆盖必须另行更新文档与验收项。
// ============================================================================

#include <nlohmann/json.hpp>

#include <string>

namespace mio {

struct ConversationLifecycleConfig {
    int silenceTimeoutSeconds = 120;
    bool summarizeOnSilence = true;
    int minMessagesBeforeSummary = 2;
    int maxPendingSummaryRetries = 3;
};

struct ConfigValidationResult {
    bool ok = false;
    std::string error;
};

ConfigValidationResult validateConversationLifecycle(const ConversationLifecycleConfig& cfg);

nlohmann::json toJson(const ConversationLifecycleConfig& cfg);

// 严格解析：类型错误或越界时抛出 std::runtime_error。
// 调用方（ConfigManager）捕获后必须整次重载失败，不得发布部分配置。
ConversationLifecycleConfig conversationLifecycleFromJson(const nlohmann::json& j);

// 局部覆盖：以 inout 当前值为基底，只覆盖 JSON 中实际出现的字段
// （局部热更新语义：缺失字段保持原值）。类型错误或越界时抛出。
void applyConversationLifecycleJson(const nlohmann::json& j,
                                    ConversationLifecycleConfig& inout);

// 环境变量覆盖（只覆盖实际存在的变量，不清空其他字段）：
//   MIO_SILENCE_TIMEOUT_SECONDS / MIO_SUMMARIZE_ON_SILENCE /
//   MIO_MIN_MESSAGES_BEFORE_SUMMARY / MIO_MAX_PENDING_SUMMARY_RETRIES
// 返回 false 并填 error 表示类型/范围非法（整次重载失败）。
bool applyConversationLifecycleEnv(ConversationLifecycleConfig& cfg, std::string* error);

} // namespace mio
