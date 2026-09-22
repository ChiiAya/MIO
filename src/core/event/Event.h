#pragma once
// ============================================================================
// MIO 程序事件（审计）
//
// Event 只记录"程序级动作"：写日记、改昵称、改备注等认知类事件，
// 追加进 EventLog（events.jsonl）用于审计/调试，【不参与上下文构建】。
// 对话流（用户/助手/工具消息）的事实源是每会话的 ChatArchive JSONL
// （data/history/<会话>.jsonl），上下文由它直接重建。
//   * seq —— 全局单调序号，由 EventLog 发放并落盘（重启从日志尾部恢复）。
// ============================================================================

#include "core/conversation/Conversation.h"
#include "core/message/Message.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mio {

// 平台媒体类型（QQ 图片/语音/视频/文件；其它平台可复用）
enum class MediaKind {
    Image,
    Audio,
    Video,
    File,
};

// 适配器解析出的媒体元数据：只描述"这条消息带了什么"，不做下载/转码。
// 真正的落盘、ASR、base64 化由各平台适配器的 MediaResolver 负责。
struct MediaAttachment {
    MediaKind kind = MediaKind::File;
    std::string url;        // 平台提供的外链（可能带时效签名）
    std::string localPath;  // 平台给出的本地绝对路径（同机部署时可直接读）
    std::string fileId;     // 平台文件 id（可用于后续 get_record 之类的回查）
    std::string fileName;   // 原始文件名
    std::string mimeType;   // 已知时填写，未知由后缀推断
    std::int64_t sizeBytes = 0;
    std::int64_t durationSec = 0; // 语音/视频时长
};

// 适配器传入的原始消息（平台层视角）；Runtime 把它转成 Msg 落入会话档案
struct IncomingMessage {
    ConversationKey conversation;
    std::string senderId;
    std::string senderName; // 传入的原始昵称/群名片（NapCat 传入）
    std::string groupId;//私聊时留空
    std::string text;
    std::string platform;  // 平台标识（"qq"/"wechat"/"console"…），上下文身份用
    std::vector<std::string> atUserIds; // 消息中 @ 提及的用户 ID 列表
    std::vector<MediaAttachment> attachments; // 媒体段元数据（解析层产出）
    // 已解析好的多模态内容（如图片的 data-uri），随 Msg.parts 进入 wire 层；
    // 由平台适配器填充，Runtime/InputBuffer 只做透传。
    std::vector<Part> parts;
};

enum class EventKind {
    DiaryWritten,     // 认知：写入日记
    NicknameChanged,  // 认知：修改昵称
    NotesChanged,     // 认知：修改备注
    SummaryApplied,   // 上下文压缩：摘要已应用
    ConfigReloaded,   // 配置热重载
    MemoryRemembered, // 记忆：存入长期记忆库
};

struct Event {
    std::uint64_t seq = 0;       // 全局单调，EventLog 发放并落盘
    EventKind kind = EventKind::DiaryWritten;
    std::int64_t createdAt = 0;  // epoch 秒
    std::string text;            // 日记文本 / 昵称变更等动作描述
};

// ---- 程序事件持久化格式（JSONL，一行一个事件；只写不读，审计用）------------
nlohmann::json eventToJson(const Event& ev);

} // namespace mio
