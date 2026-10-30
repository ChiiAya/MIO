#pragma once
// ============================================================================
// MediaResolver —— 入站媒体（图片/语音/视频/文件）落地方案
//
// 职责边界：只处理"附件 -> 可消费内容"这一段，不碰 WebSocket / Runtime。
//   * 图片：读本地文件或下载 -> base64 data-uri -> Part{Kind::Image}
//     （默认 ephemeral：只随本轮请求上送，不写进 JSONL 档案，避免历史膨胀）
//   * 语音：优先本地文件；配置了 ASR 就转写为文本追加进消息正文，
//     未配置/失败则缓存文件并给出路径提示
//   * 视频/文件：缓存到本地并给出路径提示（不做内容解析）
//
// 约定：resolve() 不抛异常。任何下载/读取/转写失败都降级为文本提示，
// 绝不因为一个坏文件让整条消息处理失败。
// ============================================================================

#include <memory>
#include <string>

#include "adapters/napcat/MediaConfig.h"
#include "core/event/Event.h"

namespace mio {

class AsrProvider;

class MediaResolver {
public:
    MediaResolver(MediaConfig cfg, std::shared_ptr<AsrProvider> asr = nullptr);

    // 就地补全 msg.parts（图片）与 msg.text（转写/缓存提示）
    void resolve(IncomingMessage& msg) const;

private:
    bool kindEnabled(MediaKind kind) const;

    // 下载附件到缓存目录，成功返回本地路径，失败返回空串
    std::string download(const MediaAttachment& attachment) const;

    MediaConfig cfg_;
    std::shared_ptr<AsrProvider> asr_;
};

} // namespace mio
