#pragma once
// ============================================================================
// OpenAI Chat Completions 兼容协议适配器
// 职责边界：
//   * 组装 HTTP 请求（消息翻译成 wire 格式、工具 schema 注入）
//   * 发送 + 重试 + 错误分类
//   * 解析响应为中性 ChatResponse
//   * 厂商归一化（normalizeQuirks 将来在这里加
//     "回传 reasoning_content"之类的兼容逻辑）
//   不负责：历史管理、压缩、工具执行 —— 都在调用方。
// ============================================================================

#include "config/openai/OpenaiConfig.h"
#include "providers/llm/Llm.h"

namespace mio {

class OpenAiCompat final : public Llm {
public:
    explicit OpenAiCompat(OpenAiConfig cfg);

    ChatResponse chat(const ChatRequest& req) override;

private:
    nlohmann::json buildPayload(const ChatRequest& req) const;
    void normalizeQuirks(nlohmann::json& payload) const;

    static ChatResponse parseResponse(const std::string& bodyText);

    OpenAiConfig cfg_;
};

} // namespace mio
