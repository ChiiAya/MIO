#pragma once
// ============================================================================
// ProposalStore —— 业务提案持久化（IProposalStore 实现，子任务 C）
//
// 硬约束（见 docs/operations/memory-system-refactor.md「强制状态与人工审阅占位符」）：
//   * submit 一律持久化为 pending；review.status = HUMAN_REVIEW_PENDING；
//     review.actor 必须为空、reviewedAt 必须为 0、approvedBy 必须为空；
//   * 携带伪造审核人字段的入参必须 INVALID_ARGUMENT 且不落盘；
//   * requestHumanReview 只读取状态，approve/reject 两个桩都返回
//     HUMAN_REVIEW_REQUIRED 且不改变任何业务状态；
//   * 拒绝 / 批准 / 过期一律保留审计记录（actor 恒为空），不物理删除。
//
// 文件格式（原子写 + schema/version）：
//   { "schema": "mio.proposals", "version": 1, "nextSeq": N,
//     "proposals": [ ... ], "audit": [ ... ], <未知根字段原样保留> }
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/contracts/ProposalContracts.h"
#include "mind/proposals/JsonStore.h"

namespace mio {

// 审计记录：时间 / 动作 / actor（当前版本必须为空）/ 理由 / 结果
struct ProposalAuditRecord {
    std::int64_t at = 0;
    std::string action;      // submit / request_human_review / approve_placeholder / ...
    std::string actor;       // 当前版本恒为空，不得写模型身份
    std::string reason;
    std::string result;      // ok / HUMAN_REVIEW_REQUIRED / superseded / STORAGE_UNAVAILABLE
    std::string proposalId;
};

class ProposalStore : public IProposalStore {
public:
    static constexpr const char* kSchema = "mio.proposals";
    static constexpr int kVersion = 1;

    explicit ProposalStore(std::filesystem::path file,
                           EpochClock clock = systemEpochClock());

    // ---- IProposalStore ----
    ToolResult submit(const Proposal& proposal) override;
    std::optional<Proposal> get(const std::string& proposalId) const override;
    std::vector<Proposal> listPending(const AccessContext& access,
                                      std::size_t limit) const override;
    ReviewResult requestHumanReview(const std::string& proposalId) override;
    ReviewResult approveByHumanPlaceholder(const std::string& proposalId) override;
    ReviewResult rejectByHumanPlaceholder(const std::string& proposalId) override;

    // ---- 过期 / 被替代（未来审阅或系统策略调用；不物理删除）----
    bool markSuperseded(const std::string& proposalId, const std::string& reason);

    // 只读审计访问器（测试与未来接线用）
    std::vector<ProposalAuditRecord> auditTrail() const;
    std::size_t size() const;

    // 存储是否处于降级（加载失败 → 拒绝覆盖原文件，避免破坏数据）
    bool degraded() const;
    std::string lastError() const;

private:
    void loadLocked();
    // 由调用方持锁；成功才提交内存状态
    bool persistLocked(std::string* error) const;
    Proposal* findLocked(const std::string& id);
    const Proposal* findLocked(const std::string& id) const;
    void auditLocked(const std::string& action, const std::string& reason,
                     const std::string& result, const std::string& proposalId);
    void loadProposal(const nlohmann::json& item, Proposal* out) const;
    nlohmann::json dumpLocked() const;

    std::filesystem::path file_;
    EpochClock clock_;

    mutable std::mutex mtx_;
    std::vector<Proposal> proposals_;          // 插入序（稳定）
    std::vector<ProposalAuditRecord> audit_;
    nlohmann::json rootExtra_ = nlohmann::json::object();
    std::uint64_t nextSeq_ = 1;
    bool degraded_ = false;
    std::string lastError_;
};

} // namespace mio
