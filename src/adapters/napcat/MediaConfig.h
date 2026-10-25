#pragma once
// ============================================================================
// 入站媒体处理配置（QQ / NapCat）
//
// 媒体类消息（图片/语音/视频/文件）在解析层只保留元数据；是否真的下载、
// 送视觉模型、走 ASR，由这里的开关决定。默认只开图片与语音，避免一个
// 陌生群友发来 100MB 视频就把磁盘写满。
// ============================================================================

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "config/asr/AsrConfig.h"

namespace mio {

struct MediaConfig {
    bool enabled = true;   // 总开关：关闭后富媒体只保留 [图片]/[语音] 文本标记
    bool images = true;    // 图片 -> base64 data-uri，随请求送视觉模型
    bool audio = true;     // 语音 -> 缓存到本地；配置了 ASR 则追加转写文本
    bool video = false;    // 视频 -> 缓存到本地 + 文本路径提示（不解析内容）
    bool files = false;    // 文件 -> 缓存到本地 + 文本路径提示（不解析内容）
    // true = 图片 base64 也写进 JSONL 档案（跨重启仍可见，但历史体积会暴涨）
    bool persistImages = false;

    // 单个文件的大小上限，同时约束下载与读取（base64 会让体积膨胀约 1/3）
    std::size_t maxFileBytes = 8u * 1024u * 1024u;
    int downloadTimeoutMs = 15'000;
    std::string cacheDir = "data/media";

    AsrConfig asr;

    // 从环境变量读取（不存在则用默认值）：
    //   MIO_MEDIA_ENABLED / MIO_MEDIA_CACHE_DIR / MIO_MEDIA_MAX_BYTES
    // ASR 相关变量见 AsrConfig::fromEnvironment（MIO_ASR_*）
    static MediaConfig fromEnvironment();
};

void to_json(nlohmann::json& j, const MediaConfig& c);
void from_json(const nlohmann::json& j, MediaConfig& c);

} // namespace mio
