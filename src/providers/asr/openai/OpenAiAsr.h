#pragma once
// ============================================================================
// OpenAI /v1/audio/transcriptions 兼容 ASR 适配器
//
// 请求：multipart/form-data {file, model[, language]}
// 响应：{"text": "..."}
// 兼容 Whisper API、以及 SiliconFlow / 通义 等提供同协议的端点。
// ============================================================================

#include "config/asr/AsrConfig.h"
#include "providers/asr/AsrProvider.h"

namespace mio {

class OpenAiAsr final : public AsrProvider {
public:
    explicit OpenAiAsr(AsrConfig cfg);

    std::string transcribeFile(const std::string& audioPath) const override;

private:
    static std::string resolveEndpoint(const std::string& baseUrl);

    AsrConfig cfg_;
};

} // namespace mio
