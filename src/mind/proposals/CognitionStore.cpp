#include "mind/proposals/CognitionStore.h"

#include <algorithm>
#include <utility>

#include "log/Log.h"

namespace mio {
namespace {

std::uint64_t seqFromCognitionId(const std::string& id) {
    const std::string prefix = "cog-";
    if (id.rfind(prefix, 0) != 0) return 0;
    try {
        return static_cast<std::uint64_t>(std::stoull(id.substr(prefix.size())));
    } catch (...) {
        return 0;
    }
}

// 模型来源判定：模型来源的记录不得写 confirmed、不得升为 confirmed
bool looksLikeModelSource(const std::string& source) {
    return source.rfind("model", 0) == 0;
}

double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

const char* kTag = "CognitionStore";

} // namespace

CognitionStore::CognitionStore(std::filesystem::path file, EpochClock clock)
    : file_(std::move(file)), clock_(std::move(clock)) {
    std::lock_guard<std::mutex> lock(mtx_);
    loadLocked();
}

void CognitionStore::loadRecord(const nlohmann::json& item,
                                CognitionRecord* out) const {
    CognitionRecord r;
    r.cognitionId = item.value("cognition_id", "");
    r.subjectId = item.value("subject_id", "");
    CognitionKind kind = CognitionKind::Impression;
    parseCognitionKind(item.value("kind", std::string("impression")), kind);
    r.kind = kind;
    r.text = item.value("text", "");
    r.source = item.value("source", "");
    const std::string st = item.value("status", std::string("inferred"));
    if (st == "observed") r.status = CognitionStatus::Observed;
    else if (st == "confirmed") r.status = CognitionStatus::Confirmed;
    else r.status = CognitionStatus::Inferred;
    r.confidence = item.value("confidence", 0.0);
    if (auto it = item.find("evidence_refs");
        it != item.end() && it->is_array())
        for (const auto& e : *it)
            if (e.is_string()) r.evidenceRefs.push_back(e.get<std::string>());
    r.createdAt = item.value("created_at", std::int64_t{0});
    r.validFrom = item.value("valid_from", std::int64_t{0});
    r.validTo = item.value("valid_to", std::int64_t{0});
    r.conversationKey = item.value("conversation_key", "");
    Visibility v = Visibility::Conversation;
    parseVisibility(item.value("visibility", std::string("conversation")), v);
    r.visibility = v;  // 未知值保持最窄
    r.legacyUnverified = item.value("legacy_unverified", false);
    *out = std::move(r);
}

void CognitionStore::loadLocked() {
    records_.clear();
    audit_.clear();
    rootExtra_ = nlohmann::json::object();
    nextSeq_ = 1;
    degraded_ = false;
    lastError_.clear();

    nlohmann::json j;
    bool exists = false;
    std::string err;
    if (!jsonstore::loadJsonFile(file_, &j, &exists, &err)) {
        if (!exists) return;
        degraded_ = true;
        lastError_ = err;
        log::error(kTag, "认知库加载失败（保留原文件，以空库继续）: " + err);
        return;
    }
    if (!j.is_object()) {
        degraded_ = true;
        lastError_ = "认知库根节点不是 JSON 对象";
        log::error(kTag, lastError_ + "（保留原文件）");
        return;
    }
    if (j.contains("schema") &&
        j.value("schema", std::string()) != std::string(kSchema)) {
        degraded_ = true;
        lastError_ = "schema 不匹配: " + j.value("schema", std::string());
        log::error(kTag, lastError_ + "（拒绝覆盖，保留原文件）");
        return;
    }
    if (j.value("version", 0) > kVersion) {
        degraded_ = true;
        lastError_ = "版本高于本程序支持: " + std::to_string(j.value("version", 0));
        log::error(kTag, lastError_ + "（拒绝覆盖，保留原文件）");
        return;
    }
    if (auto it = j.find("cognitions"); it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            CognitionRecord r;
            loadRecord(item, &r);
            if (r.cognitionId.empty())
                r.cognitionId = "cog-" + std::to_string(nextSeq_);
            nextSeq_ = std::max(nextSeq_, seqFromCognitionId(r.cognitionId) + 1);
            records_.push_back(std::move(r));
        }
    }
    if (auto it = j.find("audit"); it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            CognitionAuditRecord a;
            a.at = item.value("at", std::int64_t{0});
            a.action = item.value("action", "");
            a.actor = item.value("actor", "");
            a.reason = item.value("reason", "");
            a.result = item.value("result", "");
            a.cognitionId = item.value("cognition_id", "");
            audit_.push_back(std::move(a));
        }
    }
    nextSeq_ = std::max(nextSeq_, j.value("nextSeq", std::uint64_t{1}));
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "schema" || it.key() == "version" ||
            it.key() == "nextSeq" || it.key() == "cognitions" ||
            it.key() == "audit")
            continue;
        rootExtra_[it.key()] = it.value();
    }
}

nlohmann::json CognitionStore::dumpLocked() const {
    nlohmann::json j =
        rootExtra_.is_object() ? rootExtra_ : nlohmann::json::object();
    j["schema"] = kSchema;
    j["version"] = kVersion;
    j["nextSeq"] = nextSeq_;
    auto arr = nlohmann::json::array();
    for (const auto& r : records_) {
        arr.push_back({{"cognition_id", r.cognitionId},
                       {"subject_id", r.subjectId},
                       {"kind", toString(r.kind)},
                       {"text", r.text},
                       {"source", r.source},
                       {"status", toString(r.status)},
                       {"confidence", r.confidence},
                       {"evidence_refs", r.evidenceRefs},
                       {"created_at", r.createdAt},
                       {"valid_from", r.validFrom},
                       {"valid_to", r.validTo},
                       {"conversation_key", r.conversationKey},
                       {"visibility", toString(r.visibility)},
                       {"legacy_unverified", r.legacyUnverified}});
    }
    j["cognitions"] = std::move(arr);
    auto aud = nlohmann::json::array();
    for (const auto& a : audit_) {
        aud.push_back({{"at", a.at},
                       {"action", a.action},
                       {"actor", a.actor},
                       {"reason", a.reason},
                       {"result", a.result},
                       {"cognition_id", a.cognitionId}});
    }
    j["audit"] = std::move(aud);
    return j;
}

bool CognitionStore::persistLocked(std::string* error) {
    std::string err;
    if (!jsonstore::writeAtomic(file_, dumpLocked().dump() + "\n", &err)) {
        if (error) *error = err;
        lastError_ = err;
        return false;
    }
    return true;
}

CognitionRecord* CognitionStore::findLocked(const std::string& id) {
    for (auto& r : records_)
        if (r.cognitionId == id) return &r;
    return nullptr;
}

const CognitionRecord* CognitionStore::findLocked(const std::string& id) const {
    for (const auto& r : records_)
        if (r.cognitionId == id) return &r;
    return nullptr;
}

void CognitionStore::auditLocked(const std::string& action,
                                 const std::string& reason,
                                 const std::string& result,
                                 const std::string& cognitionId) {
    CognitionAuditRecord a;
    a.at = clock_ ? clock_() : 0;
    a.action = action;
    a.actor.clear();  // 当前版本无审核人
    a.reason = reason;
    a.result = result;
    a.cognitionId = cognitionId;
    audit_.push_back(std::move(a));
}

bool CognitionStore::insertLocked(CognitionRecord rec, CognitionStatus forced,
                                  const std::string& action, std::string* outId,
                                  std::string* error) {
    if (rec.subjectId.empty()) {
        if (error) *error = "subject_id 不能为空";
        return false;
    }
    if (rec.text.empty()) {
        if (error) *error = "认知正文不能为空";
        return false;
    }
    if (rec.source.empty()) {
        if (error) *error = "来源不能为空（所有记忆写入必须带来源）";
        return false;
    }
    if (degraded_) {
        if (error) *error = "认知库处于降级状态（原文件保留未动）: " + lastError_;
        return false;
    }
    if (forced == CognitionStatus::Confirmed &&
        looksLikeModelSource(rec.source)) {
        // 模型不得写 confirmed 认知（本判定不参与授权，只是拒绝越权写入）
        if (error) *error = "模型来源不得写 confirmed 认知: " + rec.source;
        return false;
    }
    if (forced == CognitionStatus::Observed &&
        looksLikeModelSource(rec.source) && rec.evidenceRefs.empty()) {
        if (error)
            *error = "模型来源的 observed 认知必须带证据引用（禁止凭空观察）";
        return false;
    }

    const std::int64_t now = clock_ ? clock_() : 0;
    const std::vector<CognitionRecord> prevRecords = records_;
    const std::vector<CognitionAuditRecord> prevAudit = audit_;
    const std::uint64_t prevSeq = nextSeq_;

    if (rec.cognitionId.empty()) rec.cognitionId = "cog-" + std::to_string(nextSeq_);
    nextSeq_ = std::max(nextSeq_, seqFromCognitionId(rec.cognitionId) + 1);
    rec.status = forced;
    rec.confidence = clamp01(rec.confidence);
    rec.createdAt = rec.createdAt > 0 ? rec.createdAt : now;
    rec.validFrom = rec.validFrom > 0 ? rec.validFrom : now;
    rec.validTo = 0;
    const std::string id = rec.cognitionId;
    records_.push_back(std::move(rec));
    auditLocked(action, "状态=" + std::string(toString(forced)), "ok", id);

    std::string err;
    if (!persistLocked(&err)) {
        records_ = prevRecords;
        audit_ = prevAudit;
        nextSeq_ = prevSeq;
        if (error) *error = "认知持久化失败: " + err;
        return false;
    }
    if (outId) *outId = id;
    if (error) error->clear();
    return true;
}

bool CognitionStore::observe(const CognitionRecord& rec, std::string* outId,
                             std::string* error) {
    std::lock_guard<std::mutex> lock(mtx_);
    return insertLocked(rec, CognitionStatus::Observed, "observe", outId, error);
}

bool CognitionStore::infer(const CognitionRecord& rec, std::string* outId,
                           std::string* error) {
    std::lock_guard<std::mutex> lock(mtx_);
    return insertLocked(rec, CognitionStatus::Inferred, "infer", outId, error);
}

bool CognitionStore::addConfirmedCognition(const CognitionRecord& rec,
                                           std::string* outId,
                                           std::string* error) {
    std::lock_guard<std::mutex> lock(mtx_);
    return insertLocked(rec, CognitionStatus::Confirmed, "confirm", outId, error);
}

bool CognitionStore::confirmCognition(const std::string& cognitionId,
                                      const std::string& source,
                                      std::int64_t now, std::string* error) {
    if (source.empty()) {
        if (error) *error = "确认来源不能为空";
        return false;
    }
    if (looksLikeModelSource(source)) {
        // 模型批准/确认认知属于"模型批准"越权路径：拒绝且不改变状态
        if (error) *error = "模型来源不得确认认知（需要用户明确确认或人工审核）";
        return false;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    CognitionRecord* r = findLocked(cognitionId);
    if (r == nullptr) {
        if (error) *error = "认知不存在: " + cognitionId;
        return false;
    }
    const std::vector<CognitionRecord> prevRecords = records_;
    const std::vector<CognitionAuditRecord> prevAudit = audit_;
    r->status = CognitionStatus::Confirmed;
    r->legacyUnverified = false;  // 明确确认后不再是"未验证"
    r->source = source;
    if (r->validTo != 0) r->validTo = 0;  // 确认即恢复有效
    auditLocked("confirm", "来源=" + source +
                               (now > 0 ? "" : "（时间由服务端时钟决定）"),
                "ok", cognitionId);
    std::string err;
    if (!persistLocked(&err)) {
        records_ = prevRecords;
        audit_ = prevAudit;
        if (error) *error = "认知持久化失败: " + err;
        return false;
    }
    if (error) error->clear();
    return true;
}

bool CognitionStore::retract(const std::string& cognitionId,
                             const std::string& reason, std::int64_t now,
                             std::string* error) {
    std::lock_guard<std::mutex> lock(mtx_);
    CognitionRecord* r = findLocked(cognitionId);
    if (r == nullptr) {
        if (error) *error = "认知不存在: " + cognitionId;
        return false;
    }
    const std::vector<CognitionRecord> prevRecords = records_;
    const std::vector<CognitionAuditRecord> prevAudit = audit_;
    if (r->validTo == 0) r->validTo = now > 0 ? now : (clock_ ? clock_() : 0);
    auditLocked("retract", reason, "retracted", cognitionId);
    std::string err;
    if (!persistLocked(&err)) {
        records_ = prevRecords;
        audit_ = prevAudit;
        if (error) *error = "认知持久化失败: " + err;
        return false;
    }
    if (error) error->clear();
    return true;
}

std::string CognitionStore::importLegacyField(const std::string& subjectId,
                                              CognitionKind kind,
                                              const std::string& text,
                                              const std::string& conversationKey,
                                              std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (degraded_ || subjectId.empty() || text.empty()) return "";
    CognitionRecord r;
    r.subjectId = subjectId;
    r.kind = kind;
    r.text = text;
    r.source = "legacy_graph";       // 旧图谱字段：来源可追溯
    r.status = CognitionStatus::Inferred;
    r.legacyUnverified = true;       // 不注入、不自动成为 confirmed
    r.conversationKey = conversationKey;
    r.visibility = Visibility::Conversation;  // 迁移保持最窄范围
    r.createdAt = now > 0 ? now : (clock_ ? clock_() : 0);
    r.validFrom = r.createdAt;
    std::string id;
    std::string err;
    if (!insertLocked(r, CognitionStatus::Inferred, "import_legacy", &id, &err)) {
        log::warn(kTag, "旧图谱认知迁移失败: " + err);
        return "";
    }
    return id;
}

std::vector<CognitionRecord> CognitionStore::listInjectable(
    const AccessContext& access, std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<CognitionRecord> out;
    if (limit == 0) return out;
    for (const auto& r : records_) {
        if (!injectableIntoPrompt(r.status)) continue;  // inferred 不注入
        if (r.legacyUnverified) continue;
        if (r.validTo != 0) continue;
        if (!access.canSee(r.visibility)) continue;
        if (!r.conversationKey.empty() &&
            !access.canAccessConversation(r.conversationKey))
            continue;
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(),
              [](const CognitionRecord& a, const CognitionRecord& b) {
                  if (a.subjectId != b.subjectId) return a.subjectId < b.subjectId;
                  if (a.kind != b.kind)
                      return static_cast<int>(a.kind) < static_cast<int>(b.kind);
                  return a.cognitionId < b.cognitionId;
              });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<CognitionRecord> CognitionStore::listForSubject(
    const std::string& subjectId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<CognitionRecord> out;
    for (const auto& r : records_)
        if (r.subjectId == subjectId) out.push_back(r);
    std::sort(out.begin(), out.end(),
              [](const CognitionRecord& a, const CognitionRecord& b) {
                  if (a.createdAt != b.createdAt) return a.createdAt < b.createdAt;
                  return a.cognitionId < b.cognitionId;
              });
    return out;
}

std::optional<CognitionRecord> CognitionStore::get(
    const std::string& cognitionId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const CognitionRecord* r = findLocked(cognitionId);
    if (r == nullptr) return std::nullopt;
    return *r;
}

std::vector<CognitionAuditRecord> CognitionStore::auditTrail() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return audit_;
}

std::size_t CognitionStore::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return records_.size();
}

bool CognitionStore::degraded() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return degraded_;
}

std::string CognitionStore::lastError() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastError_;
}

} // namespace mio
