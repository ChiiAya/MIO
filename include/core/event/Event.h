// event，MIO_Mindloop统一外部事件输入数据模型（类手机通知结构）
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "core/message/Message.h"

namespace mio {

enum class EventCategory {
    ExternalMessage,
    PluginNotice,
    InternalPulse,
    SystemSignal
};

enum class AttentionTier {
    DirectFull = 0, // 直接全文注入 (头信息 + content + 多模态 parts)
    Preview    = 1, // 仅注入通知头 (source + title + 事件类型，不含正文与媒体)
    Digest     = 2, // 进入堆积缓冲，满阈值后仅按来源汇总条数注入
    Silent     = 3  // 直接存档，不进入 Msg
};

const char* eventCategoryToString(EventCategory cat);
const char* attentionTierToString(AttentionTier tier);
std::optional<AttentionTier> attentionTierFromString(const std::string& s);

struct AttentionRule {
    std::string pattern;        // 匹配 source / channelId / actorId
    AttentionTier targetTier;
    std::int64_t expiresAt = 0;
    std::string reason;
    bool byModel = true;
};

struct Event {
    std::uint64_t id = 0;
    std::int64_t timestamp = 0;
    EventCategory category = EventCategory::ExternalMessage;
    AttentionTier tier = AttentionTier::Preview;

    // 手机通知核心三要素：来源App(source) + 通知标题/发送人(title) + 正文(content)
    std::string source;         // 如 "napcat", "asr", "scheduler"
    std::string title;          // 通知标题/发送者名称，如 "chiiaya" 或 "测试群 - Alice"
    std::string content;        // 完整正文（仅 DirectFull 注入，或供 Tool 按需拉取）

    // 供 AttentionRule 匹配与 Tool 反查的 ID
    std::string channelId;      // 会话/群聊 ID，如 "group:278208658"
    std::string actorId;        // 发送者 ID，如 "2955350816"

    std::vector<Part> parts;    // 全模态附件（图片/音频/视频/文件）
    nlohmann::json data = nlohmann::json::object();

    std::optional<Msg> toMsg(std::optional<AttentionTier> overrideTier = std::nullopt) const;
    static std::optional<Msg> toDigestMsg(const std::vector<Event>& events);

    nlohmann::json toJson() const;
    static Event fromJson(const nlohmann::json& j);
};

} // namespace mio
