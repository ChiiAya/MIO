#include "core/contracts/SummaryContracts.h"

namespace mio {

SummaryValidation validateSummaryRecord(const SummaryRecord& rec) {
    SummaryValidation v;
    // 强制字段：from/to/summary_kind/source/visibility 不得为空
    if (rec.conversationKey.empty()) {
        v.reason = "conversation_key 不得为空";
        return v;
    }
    if (rec.fromMessageId <= 0 || rec.toMessageId <= 0 ||
        rec.toMessageId < rec.fromMessageId) {
        v.reason = "消息范围非法：需要 0 < from_message_id <= to_message_id";
        return v;
    }
    if (rec.source.empty()) {
        v.reason = "source 不得为空";
        return v;
    }
    // summary_kind 由强类型枚举保证；字符串入口一律走 parseSummaryKind 严格解析
    if (toString(rec.kind)[0] == '\0') {
        v.reason = "summary_kind 非法";
        return v;
    }
    if (rec.summary.empty()) {
        v.reason = "summary 正文不得为空";
        return v;
    }
    if (rec.summary.size() > 64 * 1024) {
        v.reason = "summary 正文超长";
        return v;
    }
    v.ok = true;
    return v;
}

} // namespace mio
