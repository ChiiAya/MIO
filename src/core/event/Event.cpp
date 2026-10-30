#include "core/event/Event.h"
#include "core/time/LocalTime.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace mio {

namespace {

std::int64_t toSeconds(std::int64_t ts) {
    if (ts <= 0) return 0;
    return (ts > 100000000000LL) ? (ts / 1000) : ts;
}

std::string formatHeader(const Event& ev, const char* tag) {
    std::ostringstream oss;
    oss << "[" << tag;
    if (ev.id > 0) oss << " #" << ev.id;
    oss << " | " << eventCategoryToString(ev.category);
    if (!ev.source.empty())    oss << " | source:" << ev.source;
    if (!ev.channelId.empty()) oss << " | channel:" << ev.channelId;
    if (!ev.title.empty() || !ev.actorId.empty()) {
        oss << " | from:" << (ev.title.empty() ? ev.actorId : ev.title);
        if (!ev.title.empty() && !ev.actorId.empty() && ev.title != ev.actorId) {
            oss << "(" << ev.actorId << ")";
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
