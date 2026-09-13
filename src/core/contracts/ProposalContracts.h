#pragma once
// ============================================================================
// 事实 / 认知 / 关系 / 提案契约（共享，冻结）
//
// 状态机（不得混用）：
//   * 业务提案 ProposalStatus : pending / accepted / rejected / superseded
//   * 身份事实 FactStatus     : proposed / confirmed / rejected / superseded
//   * 认知 CognitionStatus    : observed / inferred / confirmed
//   * 审阅字段使用独立 ReviewPlaceholder.status
//
// 硬约束：
//   * 身份事实不原地覆盖。新值必须带来源，形成新版本或待审批提案。
//   * 模型写入先进入 proposal；confirmed 事实只能由系统初始化、用户明确确认
//     或未来人工审核服务写入 —— 模型普通工具不得写入。
//   * 任何事实删除必须转为 superseded 或 rejected，不得物理删除唯一历史版本。
//   * 默认 prompt 只注入 observed 和 confirmed 认知。
//   * 模型只能提交关系类别变化提案；关系类别是可变状态、身份事实是版本化事实。
//   * 未确认推断不得伪装成稳定事实。
// ============================================================================

#include "core/contracts/AccessContext.h"
#include "core/contracts/Errors.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mio {

// ---- 业务提案 ----
enum class ProposalKind {
    Fact,          // 身份/世界事实提案
    Preference,    // 偏好提案
    Cognition,     // 印象/认知提案
    Relationship,  // 关系类别变化提案
};

inline const char* toString(ProposalKind k) {
    switch (k) {
    case ProposalKind::Fact: return "fact";
    case ProposalKind::Preference: return "preference";
    case ProposalKind::Cognition: return "cognition";
    case ProposalKind::Relationship: return "relationship";
    }
    return "fact";
}

inline bool parseProposalKind(const std::string& s, ProposalKind& out) {
    if (s == "fact") { out = ProposalKind::Fact; return true; }
    if (s == "preference") { out = ProposalKind::Preference; return true; }
    if (s == "cognition") { out = ProposalKind::Cognition; return true; }
    if (s == "relationship") { out = ProposalKind::Relationship; return true; }
    return false;
}

enum class ProposalStatus { Pending, Accepted, Rejected, Superseded };

inline const char* toString(ProposalStatus s) {
    switch (s) {
    case ProposalStatus::Pending: return "pending";
    case ProposalStatus::Accepted: return "accepted";
    case ProposalStatus::Rejected: return "rejected";
    case ProposalStatus::Superseded: return "superseded";
    }
    return "pending";
}

enum class FactStatus { Proposed, Confirmed, Rejected, Superseded };

inline const char* toString(FactStatus s) {
    switch (s) {
    case FactStatus::Proposed: return "proposed";
    case FactStatus::Confirmed: return "confirmed";
    case FactStatus::Rejected: return "rejected";
    case FactStatus::Superseded: return "superseded";
    }
    return "proposed";
}

// 认知（偏好、印象）三态：默认 prompt 只注入 observed / confirmed
enum class CognitionStatus { Observed, Inferred, Confirmed };

inline const char* toString(CognitionStatus s) {
    switch (s) {
    case CognitionStatus::Observed: return "observed";
    case CognitionStatus::Inferred: return "inferred";
    case CognitionStatus::Confirmed: return "confirmed";
    }
    return "inferred";
}

inline bool injectableIntoPrompt(CognitionStatus s) {
    return s == CognitionStatus::Observed || s == CognitionStatus::Confirmed;
}

struct Proposal {
    std::string proposalId;      // 持久化 ID（写工具必须回传）
    ProposalKind kind = ProposalKind::Fact;
    std::string subjectId;       // 内部 ID（服务端解析；模型不得冒填）
    std::string predicate;
    std::string object;
    std::string source;          // 例如 "model_tool:memory_propose_fact"
    // 证据：服务端校验后写入真实来源（形如 "convKey#messageId"）
    std::vector<std::string> evidenceMessageIds;
    double confidence = 0.0;

    ProposalStatus status = ProposalStatus::Pending;
    FactStatus factStatus = FactStatus::Proposed;
    CognitionStatus cognitionStatus = CognitionStatus::Observed;
    ReviewPlaceholder review;    // status = HUMAN_REVIEW_PENDING，actor 空，reviewedAt 0

    std::string conversationKey;
    Visibility visibility = Visibility::Conversation;
    std::int64_t createdAt = 0;
    std::int64_t validFrom = 0;
    std::int64_t validTo = 0;
    std::string approvedBy;      // 当前版本必须为空
    // 模型在对话中声称"已确认"时仅保存声明与证据，不能授予审批权限
    std::string claimedStatus;

    bool isPending() const { return status == ProposalStatus::Pending; }
    bool hasFabricatedReviewer() const { return review.actorIsFabricated() || !approvedBy.empty(); }
};

// ---- 身份事实（版本化） ----
struct Fact {
    std::string factId;
    std::string subjectId;
    std::string predicate;
    std::string object;
    std::string source;
    double confidence = 0.0;
    FactStatus status = FactStatus::Proposed;
    std::int64_t createdAt = 0;
    std::int64_t validFrom = 0;
    std::int64_t validTo = 0;          // 0 = 仍有效
    std::string approvedBy;            // 空 = 无审批人（当前版本常态）
    std::string supersedesFactId;      // 被本版本替代的历史版本
    std::string conversationKey;
    Visibility visibility = Visibility::Conversation;
    bool legacyUnverified = false;     // 旧 personal/loves/attrs 标记，不自动成为 confirmed

    bool active() const {
        return status == FactStatus::Confirmed && validTo == 0;
    }
};

// ---- 关系状态 ----
enum class Familiarity { Stranger, Acquaintance, Familiar };

inline const char* toString(Familiarity f) {
    switch (f) {
    case Familiarity::Stranger: return "stranger";
    case Familiarity::Acquaintance: return "acquaintance";
    case Familiarity::Familiar: return "familiar";
    }
    return "stranger";
}

inline bool parseFamiliarity(const std::string& s, Familiarity& out) {
    if (s == "stranger") { out = Familiarity::Stranger; return true; }
    if (s == "acquaintance") { out = Familiarity::Acquaintance; return true; }
    if (s == "familiar") { out = Familiarity::Familiar; return true; }
    return false;
}

// 关系标签（亲属/角色关系），与"熟悉程度"分离：
//   * family 是亲属/角色关系，不是刷消息可升级的亲密度等级；
//   * blocked 是独立交互状态，不参与关系标签与熟悉度计算。
struct RelationshipState {
    std::string personId;
    std::string relationshipType;   // "" / "family" / "friend" / "partner" / "colleague"
    Familiarity familiarity = Familiarity::Stranger;
    bool blocked = false;
    double intimacyScore = 0.0;     // 事件增量 + 时间衰减，不可被普通工具任意覆盖
    double trustScore = 0.0;
    double confidence = 0.0;
    std::string source;
    std::int64_t updatedAt = 0;
    bool legacyUnverified = false;  // 旧分数不能证明朋友或亲属关系
};

// ---------------------------------------------------------------------------
// 存储接口（C 实现）
// ---------------------------------------------------------------------------
class IProposalStore {
public:
    virtual ~IProposalStore() = default;

    // 提交提案：一律持久化为 pending，绝不静默丢弃，也绝不直接改变事实层。
    // 返回 data 至少含 {proposal_id, status, review_status, kind}
    virtual ToolResult submit(const Proposal& proposal) = 0;

    virtual std::optional<Proposal> get(const std::string& proposalId) const = 0;

    // 管理员专用列表（普通模型上下文不得注入 pending 提案）
    virtual std::vector<Proposal> listPending(const AccessContext& access,
                                              std::size_t limit) const = 0;

    // 人工审阅桩：approve / reject 在人工审核服务接入前 MUST 返回
    // HUMAN_REVIEW_REQUIRED，且不得改变任何业务状态。
    virtual ReviewResult requestHumanReview(const std::string& proposalId) = 0;
    virtual ReviewResult approveByHumanPlaceholder(const std::string& proposalId) = 0;
    virtual ReviewResult rejectByHumanPlaceholder(const std::string& proposalId) = 0;
};

class IFactStore {
public:
    virtual ~IFactStore() = default;

    // 读取投影：只返回 confirmed（且未失效）的事实
    virtual std::vector<Fact> listConfirmed(const AccessContext& access,
                                            std::size_t limit) const = 0;
    virtual std::vector<Fact> listForSubject(const std::string& subjectId) const = 0;
    virtual std::optional<Fact> get(const std::string& factId) const = 0;

    // 唯一可写 confirmed 的入口：系统初始化 / 用户明确确认 / 未来人工审核服务。
    // 模型工具【不得】调用（D 的工具层不暴露此入口）。
    virtual bool addConfirmedFact(const Fact& fact, std::string* error) = 0;

    // 事实"删除" → superseded（保留历史版本，不物理删除）
    virtual bool supersedeFact(const std::string& factId, const std::string& byFactId,
                               std::string* error) = 0;
};

class IRelationshipStore {
public:
    virtual ~IRelationshipStore() = default;

    virtual RelationshipState get(const std::string& personId) const = 0;

    // 关系类别（family/friend/...）只能由自动规则或未来人工审核服务提交状态变更；
    // 普通工具仅能提交 pending 提案。
    virtual bool setRelationshipType(const std::string& personId,
                                     const std::string& relationshipType,
                                     const std::string& source, std::int64_t now,
                                     std::string* error) = 0;

    // 亲密度：事件增量 + 时间衰减（带事件来源去重）；不得被普通工具任意覆盖。
    // 当前阶段不实现"每条消息 +0.1"的晋级路径。
    virtual void bumpIntimacy(const std::string& personId, double delta,
                              const std::string& eventSource, std::int64_t now) = 0;

    virtual void setBlocked(const std::string& personId, bool blocked,
                            std::int64_t now) = 0;
};

} // namespace mio
