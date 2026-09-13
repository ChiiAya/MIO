#pragma once
// ============================================================================
// FactStore —— 身份事实版本化持久化（IFactStore 实现，子任务 C）
//
// 硬约束（见 docs/operations/memory-system-refactor.md「子任务 C」）：
//   * addConfirmedFact 是【唯一】可写 confirmed 的入口，只允许系统初始化 /
//     用户明确确认 / 未来人工审核服务调用；本类【不得】注册成模型工具；
//   * 身份事实不原地覆盖：同一 (subject_id, predicate) 的新值形成新版本，
//     由 supersedes_fact_id 链接，旧版本置 superseded 且 valid_to 落时间；
//   * 任何"删除"都转为 superseded / rejected，不得物理删除唯一历史版本；
//   * listConfirmed 只返回 confirmed && valid_to == 0 && 可见 的事实；
//   * 旧图谱迁移来的 personal/loves/attrs 只能标记 legacy_unverified，
//     不得自动成为 confirmed 事实。
//
// 文件格式（原子写 + schema/version）：
//   { "schema": "mio.facts", "version": 1, "nextSeq": N,
//     "facts": [ ... ], "audit": [ ... ] }
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

struct FactAuditRecord {
    std::int64_t at = 0;
    std::string action;   // add_confirmed / add_proposed / supersede / reject
    std::string actor;    // 当前版本恒为空
    std::string reason;
    std::string result;
    std::string factId;
};

class FactStore : public IFactStore {
public:
    static constexpr const char* kSchema = "mio.facts";
    static constexpr int kVersion = 1;

    explicit FactStore(std::filesystem::path file,
                       EpochClock clock = systemEpochClock());

    // ---- IFactStore ----
    std::vector<Fact> listConfirmed(const AccessContext& access,
                                    std::size_t limit) const override;
    std::vector<Fact> listForSubject(const std::string& subjectId) const override;
    std::optional<Fact> get(const std::string& factId) const override;
    bool addConfirmedFact(const Fact& fact, std::string* error) override;
    bool supersedeFact(const std::string& factId, const std::string& byFactId,
                       std::string* error) override;

    // 待确认事实：只能写 proposed，永不写 confirmed（模型提案走 ProposalStore，
    // 本入口用于审计/迁移场景，返回值通过 outFactId 回传）
    bool addProposedFact(const Fact& fact, std::string* outFactId,
                         std::string* error);

    // "删除"的另一条合法路径：置 rejected，保留记录
    bool rejectFact(const std::string& factId, const std::string& reason,
                    std::string* error);

    // 只读审计访问器
    std::vector<FactAuditRecord> auditTrail() const;
    std::size_t size() const;
    bool degraded() const;
    std::string lastError() const;

private:
    void loadLocked();
    void loadFact(const nlohmann::json& item, Fact* out) const;
    nlohmann::json dumpLocked() const;
    bool persistLocked(std::string* error);
    Fact* findLocked(const std::string& factId);
    const Fact* findLocked(const std::string& factId) const;
    void auditLocked(const std::string& action, const std::string& reason,
                     const std::string& result, const std::string& factId);
    // 同一 (subject, predicate) 的当前有效版本
    std::vector<Fact*> activeVersionsLocked(const std::string& subjectId,
                                            const std::string& predicate);

    std::filesystem::path file_;
    EpochClock clock_;

    mutable std::mutex mtx_;
    std::vector<Fact> facts_;
    std::vector<FactAuditRecord> audit_;
    nlohmann::json rootExtra_ = nlohmann::json::object();
    std::uint64_t nextSeq_ = 1;
    bool degraded_ = false;
    std::string lastError_;
};

} // namespace mio
