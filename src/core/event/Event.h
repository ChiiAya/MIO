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

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace mio {

// 适配器传入的原始消息（平台层视角）；Runtime 把它转成 Msg 落入会话档案
struct IncomingMessage {
    ConversationKey conversation;
    std::string senderId;
    std::string groupId;//私聊时留空
    std::string text;
    std::string platform;  // 平台标识（"qq"/"wechat"/"console"…），上下文身份用
};

enum class EventKind {
    DiaryWritten,     // 认知：写入日记
    NicknameChanged,  // 认知：修改昵称
    NotesChanged,     // 认知：修改备注
    SummaryApplied,   // 上下文压缩：摘要已应用
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
