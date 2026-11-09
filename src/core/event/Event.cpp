#include "core/event/Event.h"
#include "core/time/LocalTime.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string_view>

namespace mio {

namespace {

std::int64_t toSeconds(std::int64_t ts) {
    if (ts <= 0) return 0;
    return (ts > 100000000000LL) ? (ts / 1000) : ts;
}

// 读取 data 中的字符串字段（data 非对象或字段非字符串时返回空）
std::string dataString(const Event& ev, const char* key) {
    if (!ev.data.is_object()) return {};
    auto it = ev.data.find(key);
    if (it == ev.data.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool isAssistantEvent(const Event& ev) {
    return dataString(ev, "role") == "assistant";
}

// 聚合分组键：(category, source, channelId, actorId)
bool sameBatchGroup(const Event& a, const Event& b) {
    return a.category == b.category && a.source == b.source &&
           a.channelId == b.channelId && a.actorId == b.actorId;
}

// DirectFull 合并组内单条事件行：可选 msg_id 前缀 + 正文
std::string directFullLine(const Event& ev) {
    const std::string messageId = dataString(ev, "message_id");
    if (!messageId.empty()) return "[msg_id:" + messageId + "] " + ev.content;
    return ev.content;
}

std::string formatHeader(const Event& ev, const char* tag, bool includeId = true) {
    std::ostringstream oss;
    oss << "[" << tag;

    if (includeId) {
        // DirectFull 优先使用平台消息 ID；Preview 只暴露内部事件 ID
        if (std::string_view(tag) == "Event") {
            const std::string messageId = dataString(ev, "message_id");
            const std::string replyTo = dataString(ev, "reply_to");
            if (!messageId.empty()) {
                oss << " msg_id:" << messageId;
                if (!replyTo.empty()) oss << " | reply_to:" << replyTo;
            } else if (ev.id > 0) {
                oss << " #" << ev.id;
            }
        } else if (ev.id > 0) {
            oss << " #" << ev.id;
        }
    }

    // 外部消息是常态，省略类别段；仅系统/插件类事件显式标注
    if (ev.category != EventCategory::ExternalMessage) {
        oss << " | " << eventCategoryToString(ev.category);
    }

    if (!ev.source.empty())    oss << " | source:" << ev.source;
    if (!ev.channelId.empty()) oss << " | channel:" << ev.channelId;

    // 私聊 channelId 内已含 actorId 时不再重复输出 actor 括号
    const bool actorInChannel =
        !ev.actorId.empty() && ev.channelId.find(ev.actorId) != std::string::npos;
    if (!ev.title.empty() || !ev.actorId.empty()) {
        if (actorInChannel) {
            if (!ev.title.empty() && ev.title != ev.actorId) {
                oss << " | from:" << ev.title;
            }
        } else if (ev.title.empty()) {
            oss << " | from:" << ev.actorId;
        } else if (ev.title.find(ev.actorId) != std::string::npos) {
            oss << " | from:" << ev.title;
        } else {
            oss << " | from:" << ev.title << "(" << ev.actorId << ")";
        }
    }

    if (ev.timestamp > 0) {
        oss << " | time:" << formatLocalTime(toSeconds(ev.timestamp));
    }
    oss << "]";
    return oss.str();
}

std::optional<EventCategory> eventCategoryFromString(const std::string& s) {
    if (s == "ExternalMessage") return EventCategory::ExternalMessage;
    if (s == "PluginNotice")    return EventCategory::PluginNotice;
    if (s == "InternalPulse")   return EventCategory::InternalPulse;
    if (s == "SystemSignal")    return EventCategory::SystemSignal;
    return std::nullopt;
}

} // namespace

const char* eventCategoryToString(EventCategory cat) {
    switch (cat) {
    case EventCategory::ExternalMessage: return "ExternalMessage";
    case EventCategory::PluginNotice:    return "PluginNotice";
    case EventCategory::InternalPulse:   return "InternalPulse";
    case EventCategory::SystemSignal:    return "SystemSignal";
    }
    return "ExternalMessage";
}

const char* attentionTierToString(AttentionTier tier) {
    switch (tier) {
    case AttentionTier::DirectFull: return "DirectFull";
    case AttentionTier::Preview:    return "Preview";
    case AttentionTier::Digest:     return "Digest";
    case AttentionTier::Silent:     return "Silent";
    }
    return "Preview";
}

std::optional<AttentionTier> attentionTierFromString(const std::string& s) {
    if (s == "DirectFull" || s == "T0" || s == "full")    return AttentionTier::DirectFull;
    if (s == "Preview"    || s == "T1" || s == "preview") return AttentionTier::Preview;
    if (s == "Digest"     || s == "T2" || s == "digest")  return AttentionTier::Digest;
    if (s == "Silent"     || s == "T3" || s == "silent")  return AttentionTier::Silent;
    return std::nullopt;
}

std::optional<Msg> Event::toMsg(std::optional<AttentionTier> overrideTier) const {
    const AttentionTier activeTier = overrideTier.value_or(tier);
    if (activeTier == AttentionTier::Silent) {
        return std::nullopt;
    }

    // 机器人自身回复归档：重启后原样恢复为 assistant 消息（无 header / parts）
    if (isAssistantEvent(*this)) {
        Msg m;
        m.role = Role::Assistant;
        m.text = content;
        return m;
    }

    Msg m;
    m.role = Role::User;

    if (activeTier == AttentionTier::DirectFull) {
        m.text = formatHeader(*this, "Event") + " " + content;
        m.parts = parts;
        return m;
    }

    // Preview: 仅注入事件类型 + source + title(actor)，不含正文与多模态附件
    m.text = formatHeader(*this, "Preview");
    return m;
}

std::optional<Msg> Event::toDigestMsg(const std::vector<Event>& events) {
    std::map<std::string, std::vector<const Event*>> grouped;
    std::size_t count = 0;

    for (const auto& ev : events) {
        if (ev.tier == AttentionTier::Silent) continue;
        std::string key = !ev.channelId.empty() ? (ev.source + ":" + ev.channelId)
                        : (!ev.source.empty() ? ev.source : "unknown");
        grouped[key].push_back(&ev);
        ++count;
    }

    if (count == 0) return std::nullopt;

    std::ostringstream oss;
    oss << "[Digest | " << count << " events]";
    for (const auto& [key, list] : grouped) {
        const Event* last = list.back();
        const std::string& who = !last->title.empty() ? last->title : last->actorId;
        oss << "\n- [" << key << " | " << eventCategoryToString(last->category)
            << "] x" << list.size() << " (latest #" << last->id;
        if (!who.empty()) oss << " from:" << who;
        oss << ")";
    }

    Msg m;
    m.role = Role::User;
    m.text = oss.str();
    return m;
}

std::vector<Msg> Event::toBatchMsgs(const std::vector<Event>& events) {
    enum class PrevKind { None, Assistant, Grouped };

    std::vector<Msg> out;
    PrevKind prevKind = PrevKind::None;
    const Event* prevEvent = nullptr;

    for (const auto& ev : events) {
        if (ev.tier == AttentionTier::Silent) continue;

        if (isAssistantEvent(ev)) {
            if (prevKind == PrevKind::Assistant && prevEvent && !out.empty() &&
                out.back().role == Role::Assistant && sameBatchGroup(*prevEvent, ev)) {
                out.back().text += "\n" + ev.content;
            } else {
                Msg m{Role::Assistant};
                m.text = ev.content;
                out.push_back(std::move(m));
            }
            prevKind = PrevKind::Assistant;
            prevEvent = &ev;
            continue;
        }

        if (ev.tier == AttentionTier::DirectFull) {
            if (prevKind == PrevKind::Grouped && prevEvent && !out.empty() &&
                sameBatchGroup(*prevEvent, ev)) {
                out.back().text += "\n" + directFullLine(ev);
                out.back().parts.insert(out.back().parts.end(), ev.parts.begin(), ev.parts.end());
            } else {
                Msg m{Role::User};
                m.text = formatHeader(ev, "Event", /*includeId=*/false) + "\n" + directFullLine(ev);
                m.parts = ev.parts;
                out.push_back(std::move(m));
            }
            prevKind = PrevKind::Grouped;
            prevEvent = &ev;
            continue;
        }

        // Preview / Digest 等：逐条转换
        if (auto m = ev.toMsg()) {
            out.push_back(std::move(*m));
        }
        prevKind = PrevKind::None;
        prevEvent = nullptr;
    }

    return out;
}

nlohmann::json Event::toJson() const {
    nlohmann::json j = {
        {"id", id},
        {"timestamp", timestamp},
        {"category", eventCategoryToString(category)},
        {"tier", attentionTierToString(tier)},
    };

    if (!source.empty())    j["source"] = source;
    if (!title.empty())     j["title"] = title;
    if (!content.empty())   j["content"] = content;
    if (!channelId.empty()) j["channelId"] = channelId;
    if (!actorId.empty())   j["actorId"] = actorId;
    if (!data.empty())      j["data"] = data;

    if (!parts.empty()) {
        auto arr = nlohmann::json::array();
        for (const auto& p : parts) {
            nlohmann::json pj = {
                {"kind", partKindToString(p.kind)},
                {"data", p.data},
            };
            if (!p.mimeType.empty()) pj["mimeType"] = p.mimeType;
            arr.push_back(std::move(pj));
        }
        j["parts"] = std::move(arr);
    }

    return j;
}

Event Event::fromJson(const nlohmann::json& j) {
    Event ev;
    if (!j.is_object()) return ev;

    ev.id        = j.value("id", std::uint64_t{0});
    ev.timestamp = j.value("timestamp", std::int64_t{0});
    ev.source    = j.value("source", "");
    ev.title     = j.value("title", "");
    ev.content   = j.value("content", "");
    ev.channelId = j.value("channelId", "");
    ev.actorId   = j.value("actorId", "");

    if (auto it = j.find("category"); it != j.end() && it->is_string()) {
        ev.category = eventCategoryFromString(it->get<std::string>()).value_or(EventCategory::ExternalMessage);
    }
    if (auto it = j.find("tier"); it != j.end() && it->is_string()) {
        ev.tier = attentionTierFromString(it->get<std::string>()).value_or(AttentionTier::Preview);
    }
    if (auto it = j.find("parts"); it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
            ev.parts.push_back(Part{
                partKindFromString(item.value("kind", "image")),
                item.value("data", item.value("text", "")),
                item.value("mimeType", ""),
            });
        }
    }
    if (auto it = j.find("data"); it != j.end() && !it->is_null()) {
        ev.data = *it;
    }
    return ev;
}

} // namespace mio
