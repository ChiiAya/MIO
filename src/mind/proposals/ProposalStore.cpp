#include "mind/proposals/ProposalStore.h"

#include <algorithm>
#include <utility>

#include "log/Log.h"

namespace mio {
namespace {

// 模型来源判定：仅用于阻止模型提升可见性 / 写 confirmed；不参与任何授权判断
bool looksLikeModelSource(const std::string& source) {
    return source.rfind("model", 0) == 0;  // "model_tool:..." / "model:..."
}

std::uint64_t seqFromId(const std::string& id) {
    const std::string prefix = "prop-";
    if (id.rfind(prefix, 0) != 0) return 0;
    try {
        return static_cast<std::uint64_t>(std::stoull(id.substr(prefix.size())));
    } catch (...) {
        return 0;
    }
}

const char* kAuditTag = "ProposalStore";

} // namespace

ProposalStore::ProposalStore(std::filesystem::path file, EpochClock clock)
    : file_(std::move(file)), clock_(std::move(clock)) {
    // 构造期间无并发，直接持锁加载；加载失败只降级，不写文件
    std::lock_guard<std::mutex> lock(mtx_);
    loadLocked();
}

void ProposalStore::loadProposal(const nlohmann::json& item,
                                 Proposal* out) const {
    Proposal p;
    p.proposalId = item.value("proposal_id", "");
    ProposalKind kind = ProposalKind::Fact;
    parseProposalKind(item.value("kind", std::string("fact")), kind);
    p.kind = kind;
    p.subjectId = item.value("subject_id", "");
    p.predicate = item.value("predicate", "");
    p.object = item.value("object", "");
    p.source = item.value("source", "");
    if (auto it = item.find("evidence_message_ids");
        it != item.end() && it->is_array())
        for (const auto& e : *it)
            if (e.is_string()) p.evidenceMessageIds.push_back(e.get<std::string>());
    p.confidence = item.value("confidence", 0.0);

    const std::string status = item.value("status", std::string("pending"));
    if (status == "accepted") p.status = ProposalStatus::Accepted;
    else if (status == "rejected") p.status = ProposalStatus::Rejected;
    else if (status == "superseded") p.status = ProposalStatus::Superseded;
    else p.status = ProposalStatus::Pending;

    const std::string fs = item.value("fact_status", std::string("proposed"));
    if (fs == "confirmed") p.factStatus = FactStatus::Confirmed;
    else if (fs == "rejected") p.factStatus = FactStatus::Rejected;
    else if (fs == "superseded") p.factStatus = FactStatus::Superseded;
    else p.factStatus = FactStatus::Proposed;

    const std::string cs = item.value("cognition_status", std::string("inferred"));
    if (cs == "observed") p.cognitionStatus = CognitionStatus::Observed;
    else if (cs == "confirmed") p.cognitionStatus = CognitionStatus::Confirmed;
    else p.cognitionStatus = CognitionStatus::Inferred;

    if (auto it = item.find("review"); it != item.end() && it->is_object()) {
        p.review.status = it->value("status", "");
        p.review.actor = it->value("actor", "");
        p.review.reason = it->value("reason", "");
        p.review.reviewedAt = it->value("reviewed_at", std::int64_t{0});
    }
    p.conversationKey = item.value("conversation_key", "");
    Visibility v = Visibility::Conversation;
    parseVisibility(item.value("visibility", std::string("conversation")), v);
    p.visibility = v;  // 未知值保持最窄
    p.createdAt = item.value("created_at", std::int64_t{0});
    p.validFrom = item.value("valid_from", std::int64_t{0});
    p.validTo = item.value("valid_to", std::int64_t{0});
    p.approvedBy = item.value("approved_by", "");
    p.claimedStatus = item.value("claimed_status", "");
    // 读取期协议修复：pending 提案必须带 HUMAN_REVIEW_PENDING 审阅状态；
    // 文件里若出现伪造审核人（外部手改/未来版本写坏），只告警不采用为授权依据
    if (p.status == ProposalStatus::Pending && p.review.status.empty()) {
        p.review.status = kHumanReviewPending;
    }
    if (p.status == ProposalStatus::Pending &&
        (p.review.actorIsFabricated() || !p.approvedBy.empty())) {
        log::warn(kAuditTag,
                  "提案 " + p.proposalId +
                      " 文件内含非空审核人字段；当前版本不作为授权依据");
    }
    *out = std::move(p);
}

void ProposalStore::loadLocked() {
    proposals_.clear();
    audit_.clear();
    rootExtra_ = nlohmann::json::object();
    nextSeq_ = 1;
    degraded_ = false;
    lastError_.clear();

    nlohmann::json j;
    bool exists = false;
    std::string err;
    if (!jsonstore::loadJsonFile(file_, &j, &exists, &err)) {
        if (!exists) return;  // 首次运行：空 store
        degraded_ = true;
        lastError_ = err;
        log::error(kAuditTag, "提案库加载失败（保留原文件，以空库继续）: " + err);
        return;
    }
    if (!j.is_object()) {
        degraded_ = true;
        lastError_ = "提案库根节点不是 JSON 对象";
        log::error(kAuditTag, lastError_ + "（保留原文件）");
        return;
    }
    if (j.contains("schema") &&
        j.value("schema", std::string()) != std::string(kSchema)) {
        degraded_ = true;
        lastError_ = "schema 不匹配: " + j.value("schema", std::string());
        log::error(kAuditTag, lastError_ + "（拒绝覆盖，保留原文件）");
        return;
    }
    if (j.value("version", 0) > kVersion) {
        degraded_ = true;
        lastError_ = "版本高于本程序支持: " + std::to_string(j.value("version", 0));
        log::error(kAuditTag, lastError_ + "（拒绝覆盖，保留原文件）");
        return;
    }

    if (auto it = j.find("proposals"); it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            Proposal p;
            loadProposal(item, &p);
            if (p.proposalId.empty()) {
                p.proposalId = "prop-" + std::to_string(nextSeq_);
            }
            nextSeq_ = std::max(nextSeq_, seqFromId(p.proposalId) + 1);
            proposals_.push_back(std::move(p));
        }
    }
    if (auto it = j.find("audit"); it != j.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            ProposalAuditRecord a;
            a.at = item.value("at", std::int64_t{0});
            a.action = item.value("action", "");
            a.actor = item.value("actor", "");
            a.reason = item.value("reason", "");
            a.result = item.value("result", "");
            a.proposalId = item.value("proposal_id", "");
            audit_.push_back(std::move(a));
        }
    }
    nextSeq_ = std::max(nextSeq_, j.value("nextSeq", std::uint64_t{1}));
    // 未知根字段原样保留（向前兼容，写回不丢字段）
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "schema" || it.key() == "version" ||
            it.key() == "nextSeq" || it.key() == "proposals" ||
            it.key() == "audit")
            continue;
        rootExtra_[it.key()] = it.value();
    }
}

nlohmann::json ProposalStore::dumpLocked() const {
    nlohmann::json j = rootExtra_.is_object() ? rootExtra_
                                              : nlohmann::json::object();
    j["schema"] = kSchema;
    j["version"] = kVersion;
    j["nextSeq"] = nextSeq_;
    auto arr = nlohmann::json::array();
    for (const auto& p : proposals_) {
        nlohmann::json item;
        item["proposal_id"] = p.proposalId;
        item["kind"] = toString(p.kind);
        item["subject_id"] = p.subjectId;
        item["predicate"] = p.predicate;
        item["object"] = p.object;
        item["source"] = p.source;
        item["evidence_message_ids"] = p.evidenceMessageIds;
        item["confidence"] = p.confidence;
        item["status"] = toString(p.status);
        item["fact_status"] = toString(p.factStatus);
        item["cognition_status"] = toString(p.cognitionStatus);
        item["review"] = {{"status", p.review.status},
                          {"actor", p.review.actor},
                          {"reason", p.review.reason},
                          {"reviewed_at", p.review.reviewedAt}};
        item["conversation_key"] = p.conversationKey;
        item["visibility"] = toString(p.visibility);
        item["created_at"] = p.createdAt;
        item["valid_from"] = p.validFrom;
        item["valid_to"] = p.validTo;
        item["approved_by"] = p.approvedBy;
        item["claimed_status"] = p.claimedStatus;
        arr.push_back(std::move(item));
    }
    j["proposals"] = std::move(arr);
    auto aud = nlohmann::json::array();
    for (const auto& a : audit_) {
        aud.push_back({{"at", a.at},
                       {"action", a.action},
                       {"actor", a.actor},
                       {"reason", a.reason},
                       {"result", a.result},
                       {"proposal_id", a.proposalId}});
    }
    j["audit"] = std::move(aud);
    return j;
}

bool ProposalStore::persistLocked(std::string* error) const {
    std::string err;
    if (!jsonstore::writeAtomic(file_, dumpLocked().dump() + "\n", &err)) {
        if (error) *error = err;
        return false;
    }
    return true;
}

Proposal* ProposalStore::findLocked(const std::string& id) {
    for (auto& p : proposals_)
        if (p.proposalId == id) return &p;
    return nullptr;
}

const Proposal* ProposalStore::findLocked(const std::string& id) const {
    for (const auto& p : proposals_)
        if (p.proposalId == id) return &p;
    return nullptr;
}

void ProposalStore::auditLocked(const std::string& action,
                                const std::string& reason,
                                const std::string& result,
                                const std::string& proposalId) {
    ProposalAuditRecord a;
    a.at = clock_ ? clock_() : 0;
    a.action = action;
    a.actor.clear();  // 当前版本审核人必须为空（不得写模型身份）
    a.reason = reason;
    a.result = result;
    a.proposalId = proposalId;
    audit_.push_back(std::move(a));
}

ToolResult ProposalStore::submit(const Proposal& proposal) {
    // 1) 伪造审核人：直接拒绝，不落盘
    if (proposal.hasFabricatedReviewer()) {
        return ToolResult::failure(
            ErrorCode::InvalidArgument,
            "提案携带伪造审核人字段（review.actor / review.reviewedAt / "
            "approvedBy 当前版本必须为空），已拒绝且未落盘");
    }
    if (proposal.subjectId.empty()) {
        return ToolResult::failure(
            ErrorCode::InvalidArgument,
            "subject_id 不能为空：身份必须由服务端解析为内部 ID");
    }
    if (proposal.predicate.empty()) {
        return ToolResult::failure(ErrorCode::InvalidArgument,
                                   "predicate 不能为空");
    }
    if (proposal.visibility == Visibility::Public &&
        looksLikeModelSource(proposal.source)) {
        return ToolResult::failure(
            ErrorCode::InvalidArgument,
            "模型提交的提案不得把可见性提升为 public");
    }

    std::lock_guard<std::mutex> lock(mtx_);
    if (degraded_) {
        return ToolResult::failure(
            ErrorCode::StorageUnavailable,
            "提案库处于降级状态（原文件保留未动），拒绝写入: " + lastError_);
    }

    Proposal p = proposal;
    const std::int64_t now = clock_ ? clock_() : 0;

    // 2) 服务端权威字段强制覆盖：一律 pending、审阅占位为空
    std::vector<std::string> ignored;
    if (proposal.status != ProposalStatus::Pending)
        ignored.push_back(std::string("status=") + toString(proposal.status));
    if (!proposal.review.status.empty() &&
        proposal.review.status != kHumanReviewPending)
        ignored.push_back("review_status=" + proposal.review.status);
    if (proposal.factStatus == FactStatus::Confirmed)
        ignored.push_back("fact_status=confirmed");
    if (proposal.cognitionStatus == CognitionStatus::Confirmed)
        ignored.push_back("cognition_status=confirmed");

    p.proposalId = "prop-" + std::to_string(nextSeq_);
    p.status = ProposalStatus::Pending;
    p.factStatus = FactStatus::Proposed;   // 事实层不变，等人工审阅
    // 模型写偏好/印象只能是 inferred（明确可观察才用 observed），不得升为 confirmed
    p.cognitionStatus = proposal.cognitionStatus == CognitionStatus::Observed
                            ? CognitionStatus::Observed
                            : CognitionStatus::Inferred;
    p.review.status = kHumanReviewPending;
    p.review.actor.clear();
    p.review.reviewedAt = 0;
    p.approvedBy.clear();
    p.createdAt = now;
    p.validFrom = 0;
    p.validTo = 0;

    std::string reason;
    if (!ignored.empty()) {
        reason = "忽略调用方越权字段: ";
        for (std::size_t i = 0; i < ignored.size(); ++i) {
            if (i != 0) reason += ", ";
            reason += ignored[i];
        }
    }
    if (!proposal.claimedStatus.empty()) {
        if (!reason.empty()) reason += "; ";
        reason += "记录对话声明 claimed_status=" + proposal.claimedStatus +
                  "（声明不授予审批权限）";
    }

    // 3) 提交内存状态 → 原子落盘，失败则整体回滚（不得静默丢弃或谎报成功）
    const std::vector<Proposal> prevProposals = proposals_;
    const std::vector<ProposalAuditRecord> prevAudit = audit_;
    const std::uint64_t prevSeq = nextSeq_;
    proposals_.push_back(p);
    ++nextSeq_;
    auditLocked("submit", reason, "ok", p.proposalId);

    std::string err;
    if (!persistLocked(&err)) {
        proposals_ = prevProposals;
        audit_ = prevAudit;
        nextSeq_ = prevSeq;
        lastError_ = err;
        return ToolResult::failure(ErrorCode::StorageUnavailable,
                                   "提案持久化失败: " + err);
    }

    nlohmann::json data;
    data["proposal_id"] = p.proposalId;
    data["status"] = toString(p.status);
    data["review_status"] = p.review.status;
    data["kind"] = toString(p.kind);
    data["fact_status"] = toString(p.factStatus);
    data["cognition_status"] = toString(p.cognitionStatus);
    data["created_at"] = p.createdAt;
    data["persisted"] = true;
    return ToolResult::success(std::move(data));
}

std::optional<Proposal> ProposalStore::get(const std::string& proposalId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const Proposal* p = findLocked(proposalId);
    if (p == nullptr) return std::nullopt;
    return *p;
}

std::vector<Proposal> ProposalStore::listPending(const AccessContext& access,
                                                 std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<Proposal> out;
    // limit == 0 视为不返回（避免误用导致无界读取）；管理员显式给出上限
    if (limit == 0) return out;
    for (const auto& p : proposals_) {
        if (p.status != ProposalStatus::Pending) continue;
        if (!access.canSee(p.visibility)) continue;
        // 会话范围：默认只允许当前物理会话；跨会话需显式白名单
        if (!p.conversationKey.empty() &&
            !access.canAccessConversation(p.conversationKey))
            continue;
        out.push_back(p);
    }
    // 新的在前；createdAt 相同时保持插入序（确定性输出）
    std::stable_sort(out.begin(), out.end(),
                     [](const Proposal& a, const Proposal& b) {
                         return a.createdAt > b.createdAt;
                     });
    if (out.size() > limit) out.resize(limit);
    return out;
}

ReviewResult ProposalStore::requestHumanReview(const std::string& proposalId) {
    std::lock_guard<std::mutex> lock(mtx_);
    ReviewResult r;
    r.code = ErrorCode::HumanReviewRequired;
    const Proposal* p = findLocked(proposalId);
    if (p == nullptr) {
        // 不存在与不可见统一错误码区域语义：这里只返回稳定错误，不改变任何状态
        r.code = ErrorCode::NotFoundOrForbidden;
        r.message = "提案不存在或不可见: " + proposalId;
        return r;
    }
    // 状态保持 pending（或保持原有终态），只返回占位状态
    r.status = p->isPending() ? kHumanReviewPending : p->review.status;
    r.message = "人工审阅服务未接入：提案保持 " + std::string(toString(p->status)) +
                "，审阅状态 " + r.status;
    auditLocked("request_human_review", r.message, kHumanReviewRequired,
                proposalId);
    std::string err;
    if (!persistLocked(&err)) {
        // 审计落盘失败：不影响业务状态，但要记录（不谎报）
        lastError_ = err;
        log::warn(kAuditTag, "审阅请求审计落盘失败: " + err);
    }
    return r;
}

ReviewResult ProposalStore::approveByHumanPlaceholder(
    const std::string& proposalId) {
    std::lock_guard<std::mutex> lock(mtx_);
    ReviewResult r;
    // 硬约束：人工审核服务接入前，任何批准调用都必须返回 HUMAN_REVIEW_REQUIRED，
    // 且不得改变任何业务状态（review.status 保持 HUMAN_REVIEW_PENDING）。
    r.code = ErrorCode::HumanReviewRequired;
    const Proposal* p = findLocked(proposalId);
    if (p == nullptr) {
        r.message = "提案不存在或不可见，批准未生效: " + proposalId;
        return r;
    }
    r.status = p->review.status;
    r.message =
        "人工审核服务未接入，批准未生效（占位拒绝）: " + proposalId;
    auditLocked("approve_placeholder", r.message, kHumanReviewRequired,
                proposalId);
    std::string err;
    if (!persistLocked(&err)) {
        lastError_ = err;
        log::warn(kAuditTag, "批准占位审计落盘失败: " + err);
    }
    return r;
}

ReviewResult ProposalStore::rejectByHumanPlaceholder(
    const std::string& proposalId) {
    std::lock_guard<std::mutex> lock(mtx_);
    ReviewResult r;
    // 同上：拒绝桩也不得改变业务状态
    r.code = ErrorCode::HumanReviewRequired;
    const Proposal* p = findLocked(proposalId);
    if (p == nullptr) {
        r.message = "提案不存在或不可见，拒绝未生效: " + proposalId;
        return r;
    }
    r.status = p->review.status;
    r.message = "人工审核服务未接入，拒绝未生效（占位拒绝）: " + proposalId;
    auditLocked("reject_placeholder", r.message, kHumanReviewRequired,
                proposalId);
    std::string err;
    if (!persistLocked(&err)) {
        lastError_ = err;
        log::warn(kAuditTag, "拒绝占位审计落盘失败: " + err);
    }
    return r;
}

bool ProposalStore::markSuperseded(const std::string& proposalId,
                                   const std::string& reason) {
    std::lock_guard<std::mutex> lock(mtx_);
    Proposal* p = findLocked(proposalId);
    if (p == nullptr) return false;
    p->status = ProposalStatus::Superseded;
    // 过期后不再处于待审阅状态（不得写 HUMAN_REVIEW_APPROVED/REJECTED 冒充审阅结果）
    p->review.status.clear();
    p->review.actor.clear();
    p->review.reviewedAt = 0;
    auditLocked("supersede", reason, "superseded", proposalId);
    std::string err;
    if (!persistLocked(&err)) {
        lastError_ = err;
        log::warn(kAuditTag, "过期审计落盘失败: " + err);
        return false;
    }
    return true;
}

std::vector<ProposalAuditRecord> ProposalStore::auditTrail() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return audit_;
}

std::size_t ProposalStore::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return proposals_.size();
}

bool ProposalStore::degraded() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return degraded_;
}

std::string ProposalStore::lastError() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastError_;
}

} // namespace mio
