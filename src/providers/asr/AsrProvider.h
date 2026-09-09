#pragma once
// ============================================================================
// ASR Provider 抽象接口（预留）
//
// 预留给语音识别（Automatic Speech Recognition）服务提供商的抽象接口，
// 如 Whisper / FunASR / SenseVoice / 云厂商 ASR 服务的对接扩展。
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>

namespace mio {

class AsrProvider {
public:
    virtual ~AsrProvider() = default;
};

} // namespace mio

