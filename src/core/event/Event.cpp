// ============================================================================
// 程序事件序列化（见 core/Event.h）
// 审计格式：{"seq":N,"kind":"DiaryWritten","createdAt":...,"text":"..."}
// ============================================================================

#include "core/event/Event.h"

namespace mio {

namespace {

const char* kindToString(EventKind kind) {
    switch (kind) {
    case EventKind::DiaryWritten: return "DiaryWritten";
    case EventKind::NicknameChanged: return "NicknameChanged";
    case EventKind::NotesChanged: return "NotesChanged";
    case EventKind::SummaryApplied: return "SummaryApplied";
    }
    return "DiaryWritten";
}

} // namespace

nlohmann::json eventToJson(const Event& ev) {
    return {{"seq", ev.seq},
            {"kind", kindToString(ev.kind)},
            {"createdAt", ev.createdAt},
            {"text", ev.text}};
}

} // namespace mio
