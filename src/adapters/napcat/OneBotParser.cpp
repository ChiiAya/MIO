#include "adapters/napcat/OneBotParser.h"

#include <cctype>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace mio {
namespace napcat {

namespace {

std::string idToString(const nlohmann::json& value) {
    if (value.is_number_unsigned())
        return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer())
        return std::to_string(value.get<std::int64_t>());
    if (value.is_string()) return value.get<std::string>();
    return "";
}

std::int64_t int64Value(const nlohmann::json& value,
                        std::int64_t fallback = 0) {
    try {
        if (value.is_number_integer()) return value.get<std::int64_t>();
        if (value.is_number_unsigned()) {
            const auto n = value.get<std::uint64_t>();
            return n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                       ? fallback
                       : static_cast<std::int64_t>(n);
        }
        if (value.is_string() && !value.get<std::string>().empty())
            return std::stoll(value.get<std::string>());
    } catch (...) {
    }
    return fallback;
}

std::string idString(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it == j.end() ? std::string() : idToString(*it);
}

std::string stringValue(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it == j.end() ? std::string() : idToString(*it);
}

std::string segmentText(const nlohmann::json& seg) {
    if (!seg.is_object()) return "";
    const std::string type = stringValue(seg, "type");
    const auto dataIt = seg.find("data");
    const nlohmann::json empty = nlohmann::json::object();
    const nlohmann::json& data =
        dataIt != seg.end() && dataIt->is_object() ? *dataIt : empty;
    if (type == "text") return stringValue(data, "text");
    if (type == "at") {
        const std::string qq = idToString(data.value("qq", nlohmann::json()));
        return qq == "all" ? "@全体成员" : "@" + qq;
    }
    // IncomingMessage is text-only today; retain a readable marker for rich
    // segments so image/file/voice messages are not silently discarded when
    // they also carry no caption text.
    if (type == "image") return "[图片]";
    if (type == "record") return "[语音]";
    if (type == "video") return "[视频]";
    if (type == "file") return "[文件]";
    if (type == "face" || type == "mface") return "[表情]";
    if (type == "reply") return "[回复]";
    return "";
}

OneBotPostType postTypeFromString(const std::string& s) {
    if (s == "message") return OneBotPostType::Message;
    if (s == "notice") return OneBotPostType::Notice;
    if (s == "request") return OneBotPostType::Request;
    if (s == "meta_event") return OneBotPostType::MetaEvent;
    return OneBotPostType::Unknown;
}

OneBotSender parseSender(const nlohmann::json& j) {
    OneBotSender s;
    if (!j.is_object()) return s;
    s.userId = idString(j, "user_id");
    s.nickname = stringValue(j, "nickname");
    s.card = stringValue(j, "card");
    s.sex = stringValue(j, "sex");
    s.age = idToString(j.value("age", nlohmann::json()));
    s.role = stringValue(j, "role");
    s.area = stringValue(j, "area");
    s.level = stringValue(j, "level");
    s.title = stringValue(j, "title");
    s.groupId = idString(j, "group_id");
    return s;
}

} // namespace

OneBotEvent parseOneBotEvent(const nlohmann::json& raw) {
    OneBotEvent ev;
    if (!raw.is_object()) return ev;

    ev.rawJson = raw;
    ev.postTypeName = stringValue(raw, "post_type");
    ev.postType = postTypeFromString(ev.postTypeName);
    ev.subType = stringValue(raw, "sub_type");
    ev.selfId = idString(raw, "self_id");
    ev.userId = idString(raw, "user_id");
    ev.groupId = idString(raw, "group_id");
    ev.operatorId = idString(raw, "operator_id");
    ev.targetId = idString(raw, "target_id");
    ev.messageId = idString(raw, "message_id");
    ev.messageSeq = idString(raw, "message_seq");
    ev.time = int64Value(raw.value("time", nlohmann::json()));

    if (auto it = raw.find("sender"); it != raw.end() && it->is_object())
        ev.sender = parseSender(*it);
    if (ev.userId.empty()) ev.userId = ev.sender.userId;
    if (ev.groupId.empty()) ev.groupId = ev.sender.groupId;

    if (auto it = raw.find("anonymous"); it != raw.end() && it->is_object()) {
        OneBotAnonymous anon;
        anon.id = idString(*it, "id");
        anon.name = stringValue(*it, "name");
        anon.flag = stringValue(*it, "flag");
        ev.anonymous = std::move(anon);
    }

    switch (ev.postType) {
    case OneBotPostType::Message: {
        ev.isMessageEvent = true;
        ev.eventType = stringValue(raw, "message_type");
        ev.messageFormat = stringValue(raw, "message_format");
        ev.rawMessage = stringValue(raw, "raw_message");

        if (auto it = raw.find("message"); it != raw.end() && it->is_array()) {
            for (const auto& seg : *it) {
                if (!seg.is_object()) continue;
                OneBotMessageSegment s;
                s.type = stringValue(seg, "type");
                const auto dataIt = seg.find("data");
                if (dataIt != seg.end() && dataIt->is_object())
                    s.data = *dataIt;
                else
                    s.data = nlohmann::json::object();
                ev.segments.push_back(s);
                ev.text += segmentText(seg);
            }
        } else if (it != raw.end()) {
            ev.text = idToString(*it);
        } else {
            ev.text = ev.rawMessage;
        }
        // NapCat may provide an empty/unsupported segment array while still
        // retaining a useful raw_message fallback (notably string format).
        if (ev.text.empty() && !ev.rawMessage.empty()) ev.text = ev.rawMessage;
        break;
    }
    case OneBotPostType::Notice: {
        ev.eventType = stringValue(raw, "notice_type");
        ev.noticeType = ev.eventType;
        ev.honorType = stringValue(raw, "honor_type");
        if (raw.contains("dismiss") && raw["dismiss"].is_boolean())
            ev.dismiss = raw["dismiss"].get<bool>();
        ev.duration = int64Value(raw.value("duration", nlohmann::json()));
        ev.groupLevel = int64Value(raw.value("group_level", nlohmann::json()));
        if (auto it = raw.find("file"); it != raw.end() && it->is_object()) {
            OneBotNoticeFile f;
            f.id = idString(*it, "id");
            f.name = stringValue(*it, "name");
            f.size = int64Value(it->value("size", nlohmann::json()));
            f.busid = idString(*it, "busid");
            f.url = stringValue(*it, "url");
            ev.file = std::move(f);
        }
        break;
    }
    case OneBotPostType::Request: {
        ev.eventType = stringValue(raw, "request_type");
        ev.requestType = ev.eventType;
        ev.comment = stringValue(raw, "comment");
        ev.flag = stringValue(raw, "flag");
        break;
    }
    case OneBotPostType::MetaEvent:
        ev.eventType = stringValue(raw, "meta_event_type");
        break;
    case OneBotPostType::Unknown:
        break;
    }

    return ev;
}

std::optional<IncomingMessage> eventToIncomingMessage(const OneBotEvent& ev) {
    if (!ev.isMessageEvent) return std::nullopt;
    if (ev.userId.empty() || ev.userId == ev.selfId) return std::nullopt;
    // Runtime::IncomingMessage is text-only; rich segments are represented by
    // readable markers by segmentText so they are not silently lost.
    if (ev.text.empty()) return std::nullopt;

    IncomingMessage msg;
    if (ev.eventType == "group") {
        if (ev.groupId.empty()) return std::nullopt;
        msg.conversation = ConversationKey::groupChat(ev.groupId);
        msg.groupId = ev.groupId;
    } else if (ev.eventType == "private") {
        msg.conversation = ConversationKey::privateChat(ev.userId);
    } else {
        return std::nullopt;
    }

    msg.senderId = ev.userId;
    msg.text = ev.text;
    msg.platform = "qq";
    return msg;
}

std::string describeOneBotEvent(const OneBotEvent& ev) {
    std::ostringstream out;
    out << "post_type=" << ev.postTypeName;
    if (!ev.eventType.empty()) out << " type=" << ev.eventType;
    if (!ev.subType.empty()) out << " sub_type=" << ev.subType;
    if (!ev.selfId.empty()) out << " self_id=" << ev.selfId;
    if (!ev.userId.empty()) out << " user_id=" << ev.userId;
    if (!ev.groupId.empty()) out << " group_id=" << ev.groupId;
    if (!ev.operatorId.empty()) out << " operator_id=" << ev.operatorId;
    if (!ev.targetId.empty()) out << " target_id=" << ev.targetId;
    if (!ev.messageId.empty()) out << " message_id=" << ev.messageId;
    if (!ev.messageSeq.empty()) out << " message_seq=" << ev.messageSeq;
    if (ev.time > 0) out << " time=" << ev.time;

    if (!ev.sender.userId.empty())
        out << " sender_id=" << ev.sender.userId;
    if (!ev.sender.nickname.empty())
        out << " sender_nickname=" << ev.sender.nickname;
    if (!ev.sender.card.empty())
        out << " sender_card=" << ev.sender.card;
    if (!ev.sender.role.empty())
        out << " sender_role=" << ev.sender.role;
    if (!ev.sender.title.empty())
        out << " sender_title=" << ev.sender.title;

    if (ev.anonymous)
        out << " anonymous=" << ev.anonymous->name << "(" << ev.anonymous->id << ")";
    if (ev.file)
        out << " file=" << ev.file->name << " id=" << ev.file->id
            << " size=" << ev.file->size;
    if (!ev.honorType.empty()) out << " honor_type=" << ev.honorType;
    if (ev.dismiss) out << " dismiss=true";
    if (ev.duration > 0) out << " duration=" << ev.duration;
    if (ev.groupLevel > 0) out << " group_level=" << ev.groupLevel;
    if (!ev.comment.empty()) out << " comment=" << ev.comment;
    if (!ev.flag.empty()) out << " flag=" << ev.flag;

    if (!ev.text.empty()) out << " text=" << ev.text;
    if (!ev.rawMessage.empty()) out << " raw=" << ev.rawMessage;
    return out.str();
}

} // namespace napcat
} // namespace mio
