#pragma once
// ============================================================================
// CognitionStore —— 偏好 / 印象（认知三态）持久化（子任务 C）
//
// 硬约束（见 docs/operations/memory-system-refactor.md「子任务 C」与
// 「人工审阅与权限矩阵」）：
//   * 认知必须区分 observed / inferred / confirmed；
//   * 模型写入只能是 inferred，明确可观察才可 observed，MUST NOT 升为 confirmed；
//   * 默认 prompt 只注入 observed + confirmed（injectableIntoPrompt）；
//   * pending 提案绝不进入本 store，也绝不进入 listInjectable；
//   * 旧图谱 personal/loves/attrs 迁移用 legacy_unverified 标记：保留数据但不可注入。
//
// 文件格式（原子写 + schema/version）：
//   { "schema": "mio.cognitions", "version": 1, "nextSeq": N,
//     "cognitions": [ ... ], "audit": [ ... ] }
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

enum class CognitionKind {
    Preference,  // 偏好（喜欢/讨厌…）
    Impression,  // 印象（性格、习惯…）
};

inline const char* toString(CognitionKind k) {
    switch (k) {
    case CognitionKind::Preference: return "preference";
    case CognitionKind::Impression: return "impression";
    }
    return "impression";
}

inline bool parseCognitionKind(const std::string& s, CognitionKind& out) {
    if (s == "preference") { out = CognitionKind::Preference; return true; }
    if (s == "impression") { out = CognitionKind::Impression; return true; }
    return false;
}

struct CognitionRecord {
    std::string cognitionId;
    std::string subjectId;          // 内部 ID
    CognitionKind kind = CognitionKind::Preference;
    std::string text;
    std::string source;             // 例如 model_tool:memory_propose_preference
    CognitionStatus status = CognitionStatus::Inferred;
    double confidence = 0.0;
    std::vector<std::string> evidenceRefs;  // 服务端校验后的证据（conv#msgId）
    std::int64_t createdAt = 0;
    std::int64_t validFrom = 0;
    std::int64_t validTo = 0;       // 0 = 有效；撤回只置位，不删除
    std::string conversationKey;
    Visibility visibility = Visibility::Conversation;
    // 旧图谱迁移标记：数据保留，但不注入 prompt、不自动成为 confirmed
    bool legacyUnverified = false;

    // 可注入稳定 prompt 的判定（不含权限过滤）
    bool injectable() const {
        return injectableIntoPrompt(status) && !legacyUnverified && validTo == 0;
    }
};

struct CognitionAuditRecord {
    std::int64_t at = 0;
    std::string action;   // observe / infer / confirm / retract / import_legacy
    std::string actor;    // 当前版本恒为空
    std::string reason;
    std::string result;
    std::string cognitionId;
};

class CognitionStore {
public:
    static constexpr const char* kSchema = "mio.cognitions";
    static constexpr int kVersion = 1;

    explicit CognitionStore(std::filesystem::path file,
                            EpochClock clock = systemEpochClock());

    // 模型可观察路径：status 强制 observed，模型来源必须带证据
    bool observe(const CognitionRecord& rec, std::string* outId,
                 std::string* error);
    // 模型推断路径：status 强制 inferred（模型的唯一常规入口）
    bool infer(const CognitionRecord& rec, std::string* outId, std::string* error);
    // 系统初始化 / 用户明确确认 / 未来人工审核：唯一可写 confirmed 的入口
    // （不注册成模型工具；模型来源一律拒绝）
    bool addConfirmedCognition(const CognitionRecord& rec, std::string* outId,
                               std::string* error);
    // 由用户/系统把已有记录升级为 confirmed（模型来源一律拒绝）
    bool confirmCognition(const std::string& cognitionId,
                          const std::string& source, std::int64_t now,
                          std::string* error);
    // 撤回：valid_to 置位，记录保留（可审计）
    bool retract(const std::string& cognitionId, const std::string& reason,
                 std::int64_t now, std::string* error);
    // 旧图谱 personal/loves 迁移：写入 legacy_unverified 的 inferred 记录，不注入
    std::string importLegacyField(const std::string& subjectId, CognitionKind kind,
                                  const std::string& text,
                                  const std::string& conversationKey,
                                  std::int64_t now);

    // 默认 prompt 注入投影：observed + confirmed、可见、未撤回、非 legacy_unverified
    std::vector<CognitionRecord> listInjectable(const AccessContext& access,
                                                std::size_t limit) const;
    // 审计/追溯投影：某 subject 的全部记录（含 inferred / 撤回 / legacy）
    std::vector<CognitionRecord> listForSubject(const std::string& subjectId) const;
    std::optional<CognitionRecord> get(const std::string& cognitionId) const;

    std::vector<CognitionAuditRecord> auditTrail() const;
    std::size_t size() const;
    bool degraded() const;
    std::string lastError() const;

private:
    void loadLocked();
    void loadRecord(const nlohmann::json& item, CognitionRecord* out) const;
    nlohmann::json dumpLocked() const;
    bool persistLocked(std::string* error);
    CognitionRecord* findLocked(const std::string& id);
    const CognitionRecord* findLocked(const std::string& id) const;
    void auditLocked(const std::string& action, const std::string& reason,
                     const std::string& result, const std::string& cognitionId);
    bool insertLocked(CognitionRecord rec, CognitionStatus forced,
                      const std::string& action, std::string* outId,
                      std::string* error);

    std::filesystem::path file_;
    EpochClock clock_;

    mutable std::mutex mtx_;
    std::vector<CognitionRecord> records_;
    std::vector<CognitionAuditRecord> audit_;
    nlohmann::json rootExtra_ = nlohmann::json::object();
    std::uint64_t nextSeq_ = 1;
    bool degraded_ = false;
    std::string lastError_;
};

} // namespace mio
