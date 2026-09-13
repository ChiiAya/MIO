#include "mind/proposals/FactStore.h"

#include <algorithm>
#include <utility>

#include "log/Log.h"

namespace mio {
namespace {

std::uint64_t seqFromFactId(const std::string& id) {
    const std::string prefix = "fact-";
    if (id.rfind(prefix, 0) != 0) return 0;
    try {
        return static_cast<std::uint64_t>(std::stoull(id.substr(prefix.size())));
    } catch (...) {
        return 0;
    }
}

bool looksLikeReviewPlaceholder(const std::string& s) {
    return s.find("HUMAN_REVIEW_") != std::string::npos;
}

const char* kTag = "FactStore";

double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

} // namespace

FactStore::FactStore(std::filesystem::path file, EpochClock clock)
    : file_(std::move(file)), clock_(std::move(clock)) {
    std::lock_guard<std::mutex> lock(mtx_);
    loadLocked();
}

void FactStore::loadFact(const nlohmann::json& item, Fact* out) const {
    Fact f;
    f.factId = item.value("fact_id", "");
    f.subjectId = item.value("subject_id", "");
    f.predicate = item.value("predicate", "");
    f.object = item.value("object", "");
    f.source = item.value("source", "");
    f.confidence = item.value("confidence", 0.0);
    const std::string st = item.value("status", std::string("proposed"));
    if (st == "confirmed") f.status = FactStatus::Confirmed;
    else if (st == "rejected") f.status = FactStatus::Rejected;
    else if (st == "superseded") f.status = FactStatus::Superseded;
    else f.status = FactStatus::Proposed;
    f.createdAt = item.value("created_at", std::int64_t{0});
    f.validFrom = item.value("valid_from", std::int64_t{0});
    f.validTo = item.value("valid_to", std::int64_t{0});
    f.approvedBy = item.value("approved_by", "");
    f.supersedesFactId = item.value("supersedes_fact_id", "");
    f.conversationKey = item.value("conversation_key", "");
    Visibility v = Visibility::Conversation;
    parseVisibility(item.value("visibility", std::string("conversation")), v);
    f.visibility = v;  // 未知值保持最窄
    f.legacyUnverified = item.value("legacy_unverified", false);
    *out = std::move(f);
}

void FactStore::loadLocked() {
    facts_.clear();
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
        log::error(kTag, "事实库加载失败（保留原文件，以空库继续）: " + err);
        return;
    }
    if (!j.is_object()) {
        degraded_ = true;
        lastError_ = "事实库根节点不是 JSON 对象";
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
    if (auto it = j.find("facts"); it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            Fact f;
            loadFact(item, &f);
            if (f.factId.empty()) f.factId = "fact-" + std::to_string(nextSeq_);
            nextSeq_ = std::max(nextSeq_, seqFromFactId(f.factId) + 1);
            facts_.push_back(std::move(f));
        }
    }
    if (auto it = j.find("audit"); it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            FactAuditRecord a;
            a.at = item.value("at", std::int64_t{0});
            a.action = item.value("action", "");
            a.actor = item.value("actor", "");
            a.reason = item.value("reason", "");
            a.result = item.value("result", "");
            a.factId = item.value("fact_id", "");
            audit_.push_back(std::move(a));
        }
    }
    nextSeq_ = std::max(nextSeq_, j.value("nextSeq", std::uint64_t{1}));
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "schema" || it.key() == "version" ||
            it.key() == "nextSeq" || it.key() == "facts" ||
            it.key() == "audit")
            continue;
        rootExtra_[it.key()] = it.value();
    }
}

nlohmann::json FactStore::dumpLocked() const {
    nlohmann::json j =
        rootExtra_.is_object() ? rootExtra_ : nlohmann::json::object();
    j["schema"] = kSchema;
    j["version"] = kVersion;
    j["nextSeq"] = nextSeq_;
    auto arr = nlohmann::json::array();
    for (const auto& f : facts_) {
        arr.push_back({{"fact_id", f.factId},
                       {"subject_id", f.subjectId},
                       {"predicate", f.predicate},
                       {"object", f.object},
                       {"source", f.source},
                       {"confidence", f.confidence},
                       {"status", toString(f.status)},
                       {"created_at", f.createdAt},
                       {"valid_from", f.validFrom},
                       {"valid_to", f.validTo},
                       {"approved_by", f.approvedBy},
                       {"supersedes_fact_id", f.supersedesFactId},
                       {"conversation_key", f.conversationKey},
                       {"visibility", toString(f.visibility)},
                       {"legacy_unverified", f.legacyUnverified}});
    }
    j["facts"] = std::move(arr);
    auto aud = nlohmann::json::array();
    for (const auto& a : audit_) {
        aud.push_back({{"at", a.at},
                       {"action", a.action},
                       {"actor", a.actor},
                       {"reason", a.reason},
                       {"result", a.result},
                       {"fact_id", a.factId}});
    }
    j["audit"] = std::move(aud);
    return j;
}

bool FactStore::persistLocked(std::string* error) {
    std::string err;
    if (!jsonstore::writeAtomic(file_, dumpLocked().dump() + "\n", &err)) {
        if (error) *error = err;
        lastError_ = err;
        return false;
    }
    return true;
}

Fact* FactStore::findLocked(const std::string& factId) {
    for (auto& f : facts_)
        if (f.factId == factId) return &f;
    return nullptr;
}

const Fact* FactStore::findLocked(const std::string& factId) const {
    for (const auto& f : facts_)
        if (f.factId == factId) return &f;
    return nullptr;
}

void FactStore::auditLocked(const std::string& action, const std::string& reason,
                            const std::string& result,
                            const std::string& factId) {
    FactAuditRecord a;
    a.at = clock_ ? clock_() : 0;
    a.action = action;
    a.actor.clear();  // 当前版本无审核人，禁止伪造
    a.reason = reason;
    a.result = result;
    a.factId = factId;
    audit_.push_back(std::move(a));
}

std::vector<Fact*> FactStore::activeVersionsLocked(
    const std::string& subjectId, const std::string& predicate) {
    std::vector<Fact*> out;
    for (auto& f : facts_) {
        if (f.subjectId == subjectId && f.predicate == predicate &&
            f.status == FactStatus::Confirmed && f.validTo == 0)
            out.push_back(&f);
    }
    return out;
}

bool FactStore::addConfirmedFact(const Fact& fact, std::string* error) {
    if (fact.subjectId.empty() || fact.predicate.empty()) {
        if (error) *error = "subject_id / predicate 不能为空";
        return false;
    }
    if (!fact.approvedBy.empty() && looksLikeReviewPlaceholder(fact.approvedBy)) {
        if (error) *error = "approved_by 不得写人工审阅占位符";
        return false;
    }
    if (fact.status != FactStatus::Confirmed && fact.status != FactStatus::Proposed) {
        // 入口语义已定：写入即 confirmed，不允许用本入口写 rejected/superseded
        if (error) *error = "addConfirmedFact 不接受 rejected/superseded 状态";
        return false;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    if (degraded_) {
        if (error) *error = "事实库处于降级状态（原文件保留未动）: " + lastError_;
        return false;
    }
    const std::int64_t now = clock_ ? clock_() : 0;

    // 版本化：同一 (subject, predicate) 已有有效版本
    std::vector<Fact*> actives = activeVersionsLocked(fact.subjectId, fact.predicate);
    for (const Fact* a : actives) {
        if (a->object == fact.object) {
            // 值相同 → 幂等，不产生新版本、不原地覆盖
            if (error) error->clear();
            return true;
        }
    }

    const std::vector<Fact> prevFacts = facts_;
    const std::vector<FactAuditRecord> prevAudit = audit_;
    const std::uint64_t prevSeq = nextSeq_;

    Fact f = fact;
    if (f.factId.empty()) f.factId = "fact-" + std::to_string(nextSeq_);
    nextSeq_ = std::max(nextSeq_, seqFromFactId(f.factId) + 1);
    f.status = FactStatus::Confirmed;   // 本入口唯一可写 confirmed
    f.confidence = clamp01(f.confidence);
    f.createdAt = f.createdAt > 0 ? f.createdAt : now;
    f.validFrom = f.validFrom > 0 ? f.validFrom : now;
    f.validTo = 0;
    // legacy_unverified 由调用方决定：旧图谱字段即使被写成 confirmed 也保持"未验证"，
    // 并且被 listConfirmed 过滤，绝不因字段存在就升级为可注入事实。
    f.legacyUnverified = fact.legacyUnverified;

    std::string reason;
    for (Fact* a : actives) {
        // 旧版本不删除，只置 superseded 并封口
        a->status = FactStatus::Superseded;
        a->validTo = a->validTo > 0 ? a->validTo : now;
        f.supersedesFactId = a->factId;
        reason += "替代旧版本 " + a->factId + "(object=" + a->object + "); ";
    }
    const std::string newId = f.factId;
    facts_.push_back(std::move(f));
    auditLocked("add_confirmed",
                reason.empty() ? std::string("新增确认事实") : reason, "ok",
                newId);

    std::string err;
    if (!persistLocked(&err)) {
        facts_ = prevFacts;
        audit_ = prevAudit;
        nextSeq_ = prevSeq;
        if (error) *error = "事实持久化失败: " + err;
        return false;
    }
    if (error) error->clear();
    return true;
}

bool FactStore::addProposedFact(const Fact& fact, std::string* outFactId,
                                std::string* error) {
    if (fact.subjectId.empty() || fact.predicate.empty()) {
        if (error) *error = "subject_id / predicate 不能为空";
        return false;
    }
    if (!fact.approvedBy.empty()) {
        // proposed 层不允许任何审批人字段
        if (error) *error = "proposed 事实不得携带 approved_by";
        return false;
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (degraded_) {
        if (error) *error = "事实库处于降级状态: " + lastError_;
        return false;
    }
    const std::int64_t now = clock_ ? clock_() : 0;
    const std::vector<Fact> prevFacts = facts_;
    const std::vector<FactAuditRecord> prevAudit = audit_;
    const std::uint64_t prevSeq = nextSeq_;

    Fact f = fact;
    if (f.factId.empty()) f.factId = "fact-" + std::to_string(nextSeq_);
    nextSeq_ = std::max(nextSeq_, seqFromFactId(f.factId) + 1);
    f.status = FactStatus::Proposed;    // 永不写 confirmed
    f.confidence = clamp01(f.confidence);
    f.createdAt = f.createdAt > 0 ? f.createdAt : now;
    f.validFrom = 0;
    f.validTo = 0;
    f.approvedBy.clear();
    const std::string newId = f.factId;
    facts_.push_back(std::move(f));
    auditLocked("add_proposed", "写入待确认事实（事实层不变）", "ok", newId);

    std::string err;
    if (!persistLocked(&err)) {
        facts_ = prevFacts;
        audit_ = prevAudit;
        nextSeq_ = prevSeq;
        if (error) *error = "事实持久化失败: " + err;
        return false;
    }
    if (outFactId) *outFactId = newId;
    if (error) error->clear();
    return true;
}

bool FactStore::supersedeFact(const std::string& factId,
                              const std::string& byFactId,
                              std::string* error) {
    std::lock_guard<std::mutex> lock(mtx_);
    Fact* f = findLocked(factId);
    if (f == nullptr) {
        if (error) *error = "事实不存在: " + factId;
        return false;
    }
    const std::int64_t now = clock_ ? clock_() : 0;
    const std::vector<Fact> prevFacts = facts_;
    const std::vector<FactAuditRecord> prevAudit = audit_;

    f->status = FactStatus::Superseded;
    if (f->validTo == 0) f->validTo = now;
    std::string reason = "由系统/人工流程置为 superseded（历史版本保留）";
    if (!byFactId.empty()) {
        if (Fact* by = findLocked(byFactId); by != nullptr) {
            if (by->supersedesFactId.empty()) by->supersedesFactId = factId;
            reason = "被 " + byFactId + " 替代";
        } else {
            reason += "；by_fact_id 不存在: " + byFactId;
        }
    }
    auditLocked("supersede", reason, "superseded", factId);

    std::string err;
    if (!persistLocked(&err)) {
        facts_ = prevFacts;
        audit_ = prevAudit;
        if (error) *error = "事实持久化失败: " + err;
        return false;
    }
    if (error) error->clear();
    return true;
}

bool FactStore::rejectFact(const std::string& factId, const std::string& reason,
                           std::string* error) {
    std::lock_guard<std::mutex> lock(mtx_);
    Fact* f = findLocked(factId);
    if (f == nullptr) {
        if (error) *error = "事实不存在: " + factId;
        return false;
    }
    const std::int64_t now = clock_ ? clock_() : 0;
    const std::vector<Fact> prevFacts = facts_;
    const std::vector<FactAuditRecord> prevAudit = audit_;
    f->status = FactStatus::Rejected;   // 不物理删除
    if (f->validTo == 0) f->validTo = now;
    auditLocked("reject", reason, "rejected", factId);
    std::string err;
    if (!persistLocked(&err)) {
        facts_ = prevFacts;
        audit_ = prevAudit;
        if (error) *error = "事实持久化失败: " + err;
        return false;
    }
    if (error) error->clear();
    return true;
}

std::vector<Fact> FactStore::listConfirmed(const AccessContext& access,
                                           std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Fact> out;
    if (limit == 0) return out;
    for (const auto& f : facts_) {
        if (f.status != FactStatus::Confirmed || f.validTo != 0) continue;
        if (f.legacyUnverified) continue;  // 未验证字段永不作为确认事实注入
        if (!access.canSee(f.visibility)) continue;
        if (!f.conversationKey.empty() &&
            !access.canAccessConversation(f.conversationKey))
            continue;
        out.push_back(f);
    }
    // 确定性排序：subject → predicate → createdAt → fact_id
    std::sort(out.begin(), out.end(), [](const Fact& a, const Fact& b) {
        if (a.subjectId != b.subjectId) return a.subjectId < b.subjectId;
        if (a.predicate != b.predicate) return a.predicate < b.predicate;
        if (a.createdAt != b.createdAt) return a.createdAt < b.createdAt;
        return a.factId < b.factId;
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<Fact> FactStore::listForSubject(const std::string& subjectId) const {
    // 审计/追溯入口：返回该 subject 的【全部版本】（含 superseded / rejected），
    // 因此不适用可见性过滤；调用方不得直接把它当作模型可读投影
    // （模型读路径必须用 listConfirmed(access, limit)）。
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Fact> out;
    for (const auto& f : facts_)
        if (f.subjectId == subjectId) out.push_back(f);
    std::sort(out.begin(), out.end(), [](const Fact& a, const Fact& b) {
        if (a.createdAt != b.createdAt) return a.createdAt < b.createdAt;
        return a.factId < b.factId;
    });
    return out;
}

std::optional<Fact> FactStore::get(const std::string& factId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const Fact* f = findLocked(factId);
    if (f == nullptr) return std::nullopt;
    return *f;
}

std::vector<FactAuditRecord> FactStore::auditTrail() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return audit_;
}

std::size_t FactStore::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return facts_.size();
}

bool FactStore::degraded() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return degraded_;
}

std::string FactStore::lastError() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastError_;
}

} // namespace mio
