#pragma once
// ============================================================================
// ASR Provider 抽象接口
//
// 语音识别（Automatic Speech Recognition）抽象：输入本地音频文件，
// 输出转写文本。实现方可以是 OpenAI 兼容 /v1/audio/transcriptions
// （Whisper / SenseVoice / 云厂商），也可以是本地 FunASR 服务。
//
// 约定：失败抛 std::runtime_error；静音/空结果返回空串（不算失败）。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace mio {

class AsrProvider {
public:
    virtual ~AsrProvider() = default;

    // audioPath 必须是可读的本地文件；返回识别文本（UTF-8）。
    virtual std::string transcribeFile(const std::string& audioPath) const = 0;
};

} // namespace mio
