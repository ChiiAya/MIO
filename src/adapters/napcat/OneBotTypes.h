#pragma once
// ============================================================================
// NapCat / OneBot 11 事件模型
//
// 与 NapCatAdapter 解耦：这里只定义“从 OneBot JSON 解析出来的结构化事件”，
// 不负责 WebSocket、鉴权、发送、Runtime 调度。后续要消费 notice/request/
// 好友/群管等事件时，直接读这些字段即可。
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/event/Event.h"

namespace mio {
namespace napcat {

enum class OneBotPostType {
    Unknown,
    Message,
    Notice,
    Request,
    MetaEvent,
};

struct OneBotSender {
    std::string userId;    // 发送者 QQ（sender.user_id）
    std::string nickname;  // 昵称
    std::string card;      // 群名片/备注
    std::string sex;       // male / female / unknown
    std::string age;       // 年龄（NapCat 可能给 int 或 string，统一 string）
    std::string role;      // owner / admin / member（群消息）
    std::string area;      // 地区（旧协议字段）
    std::string level;     // 群等级/头衔等级
    std::string title;     // 群头衔
    std::string groupId;   // sender.group_id（部分 NapCat 扩展会带）
};

struct OneBotAnonymous {
    std::string id;
    std::string name;
    std::string flag;
};

struct OneBotNoticeFile {
    std::string id;
    std::string name;
    std::int64_t size = 0;
    std::string busid;
    std::string url;
};

struct OneBotMessageSegment {
    std::string type;
    nlohmann::json data;  // 保留完整 data，后续要图片/文件/回复等直接取
};

struct OneBotEvent {
    // ---- 通用框架字段 ----
    OneBotPostType postType = OneBotPostType::Unknown;
    std::string postTypeName;  // message / notice / request / meta_event
    std::string eventType;     // message_type / notice_type / request_type / meta_event_type
    std::string subType;       // sub_type
    std::string selfId;        // 机器人自身 QQ
    std::string userId;        // 操作/发送者 user_id（notice 中为被动者）
    std::string groupId;       // 群号，私聊为空
    std::string operatorId;    // notice 操作者 operator_id
    std::string targetId;      // notice 目标 target_id
    std::string messageId;     // message_id
    std::string messageSeq;    // message_seq
    std::int64_t time = 0;     // 事件时间戳

    // ---- message 事件 ----
    bool isMessageEvent = false;
    std::string messageFormat;      // array / string
    std::string rawMessage;         // raw_message
    std::string text;               // 拼接后的纯文本（text 段 + @qq）
    std::vector<OneBotMessageSegment> segments;
    std::vector<MediaAttachment> attachments; // image/record/video/file 段的结构化元数据
    OneBotSender sender;
    std::optional<OneBotAnonymous> anonymous;

    // ---- notice 事件专用 ----
    std::string noticeType;         // group_upload / group_admin / group_recall ...
    std::optional<OneBotNoticeFile> file;
    std::string honorType;          // group_notify 荣誉
    bool dismiss = false;           // 群解散
    std::int64_t duration = 0;      // 禁言时长秒
    std::int64_t groupLevel = 0;    // 群级别等扩展数值

    // ---- request 事件 ----
    std::string requestType;        // friend / group
    std::string comment;
    std::string flag;

    // 原始 JSON 完整保留，供未来新字段/排障使用
    nlohmann::json rawJson;
};

} // namespace napcat
} // namespace mio
