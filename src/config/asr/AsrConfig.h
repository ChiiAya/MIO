#pragma once
// ============================================================================
// ASR（语音转写）端点配置
//
// 与 EmbeddingConfig 同款：允许语音识别单独指向别的供应商（如 chat 走本地
// 模型、ASR 走云端 whisper）。baseUrl 只填到 /v1 之前，适配器自动补全
// /v1/audio/transcriptions。
// ============================================================================

#include <string>

#include <nlohmann/json.hpp>

namespace mio {

struct AsrConfig {
    bool enabled = false;          // 关闭时语音只缓存文件、不转写
    std::string baseUrl;           // 空 = 回退到 chat 的 baseUrl（MIO_BASE_URL）
    std::string apiKey;
    std::string model = "whisper-1";
    std::string language;          // 可选（如 "zh"）；空 = 服务端自动识别
    int httpTimeoutMs = 60'000;    // 转写比向量化慢，给足重试时间

    // 从环境变量读取（不存在则用默认值）：
    //   MIO_ASR_ENABLED / MIO_ASR_BASE_URL / MIO_ASR_API_KEY /
    //   MIO_ASR_MODEL / MIO_ASR_LANGUAGE
    // 未显式配置 baseUrl/apiKey 时回退到 chat 的 MIO_BASE_URL / MIO_API_KEY
    static AsrConfig fromEnvironment();
};

void to_json(nlohmann::json& j, const AsrConfig& c);
void from_json(const nlohmann::json& j, AsrConfig& c);

} // namespace mio
