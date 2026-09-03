#pragma once

#include <string>

namespace mio {

// Embedding（向量化）端点配置 —— 与 OpenAiConfig 同构但独立：
// chat 与 embedding 允许指向不同供应商（如 chat 走本地大模型、
// embedding 走 SiliconFlow 的 BGE-M3）。
struct EmbeddingConfig {
    // 归一化规则与 OpenAiConfig 相同：只填到 /v1 之前，
    // 适配器自动补全 /v1/embeddings
    std::string baseUrl = "http://127.0.0.1:8080";
    std::string apiKey;
    std::string model = "BAAI/bge-m3";  // 1024 维；OpenAI 兼容端点通用
    int maxRetries = 2;                // 额外重试次数（首次之外）
    int retryInitialDelayMs = 1500;    // 指数退避初始延迟
    double retryBackoffFactor = 2.0;
    int httpTimeoutMs = 10'000;        // 向量化是短请求；写入路径在路由锁内触发，
                                       // 超时不宜长（失败有 60s 冷却兜底）

    // 从环境变量读取（不存在则用默认值）：
    //   MIO_EMBED_BASE_URL / MIO_EMBED_API_KEY / MIO_EMBED_MODEL
    // 未显式配置时回退到 chat 的 MIO_BASE_URL / MIO_API_KEY（同端点部署最常见）
    static EmbeddingConfig fromEnvironment();
};

} // namespace mio
