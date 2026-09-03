#pragma once
// ============================================================================
// OpenAI /v1/embeddings 兼容协议适配器（BGE-M3 等向量化端点）
// 职责边界（与 OpenAiCompat 对齐）：
//   * 组装 HTTP 请求（POST {model, input}，encoding_format=float）
//   * 发送 + 重试 + 错误分类（重试策略与 OpenAiCompat 相同）
//   * 解析响应为 float 向量（data[].embedding，按 index 排序拼接）
//   不负责：归一化、维度校验、缓存 —— 都在调用方（各消费域自己把关）。
// ============================================================================

#include "config/embedding/EmbeddingConfig.h"
#include "llm/Embedding.h"

namespace mio {

class OpenAiEmbedding final : public Embedding {
public:
    explicit OpenAiEmbedding(EmbeddingConfig cfg);

    std::vector<float> embed(const std::string& text) const override;

private:
    static std::string resolveEndpoint(const std::string& baseUrl);

    EmbeddingConfig cfg_;
};

} // namespace mio
