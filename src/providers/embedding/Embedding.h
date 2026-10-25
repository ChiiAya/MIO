#pragma once
// ============================================================================
// Embedding 抽象接口（文本向量化）
//
//
// 现阶段只需要一个实现：OpenAiEmbedding（OpenAI /v1/embeddings 兼容协议），
// 覆盖 BGE-M3 / bge-large-zh / text-embedding-3 等兼容端点
// （SiliconFlow / vLLM / infinity / Ollama(/v1) / LM Studio 等）。
// ============================================================================

#include <string>
#include <vector>

namespace mio {

class Embedding {
public:
    virtual ~Embedding() = default;

    // 同步阻塞调用一次向量化，返回该文本的向量（维度由端点模型决定，
    // 调用方按需校验维度与归一化）。失败抛 std::runtime_error。
    // v1 只提供单文本：记忆写入与话题匹配都是逐条触发，批量版等真实
    // 场景出现再加，不提前塞进虚方法。
    virtual std::vector<float> embed(const std::string& text) const = 0;
};

} // namespace mio
