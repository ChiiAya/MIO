#pragma once
// ============================================================================
// MemoryStore（记忆仓库）：SQLite 原生 BLOB 向量存储 + SQL 元数据预过滤
//
// 职责边界（只存/取，不算分）：
//   * 表结构：embedding 为 float[] 的 native endian 序列化（本机库本机读，
//     不跨端交换），随摘要正文与权限元数据一行落盘；
//   * 写入：writeSummaryTx 在【同一个 SQLite 事务】内写
//     摘要行 + 摘要游标 + 向量化待办 + 提案待投递记录；
//     档案 jsonl 是 F 的已确认落盘数据，不参与本事务（不得声称二者原子）；
//   * 幂等：范围唯一键 (conv_key, from_message_id, to_message_id, summary_kind)
//     用【部分唯一索引】（WHERE from_message_id > 0 AND summary_kind <> 'manual'）
//     实现，避免和旧行（from/to = 0）以及 manual 记忆（走 idempotency_key）冲突；
//     旧 UNIQUE(conv_key, summary) 在迁移时重建表去掉，不再作为幂等依据；
//   * 读取：selectCandidates 在 SQL 层完成可见性裁剪（conversation/person/public
//     + 当前会话 + 归属人）与默认排除 context_compaction，再交给 MemoryManager
//     做向量打分；
//   * 迁移：PRAGMA user_version 记录 schema 版本；旧库按列存在性判断并重建表，
//     幂等可重试；旧 is_public=1 行收窄为 conversation 并写 migration_note +
//     migration_audit（保留 is_public 原值作为迁移证据，不删除旧行、不丢正文）。
//
// 线程安全：内部互斥；sqlite3 连接仅在本对象内使用。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/contracts/Errors.h"
#include "core/contracts/ProposalContracts.h"
#include "core/contracts/SummaryContracts.h"
#include "core/contracts/Visibility.h"

struct sqlite3;  // ::sqlite3 前置声明（完整定义在 <sqlite3.h>，仅实现可见）

namespace mio {

// 一条已落库的记忆（不含向量；给展示/工具层用）
struct MemoryRecord {
    std::int64_t id = 0;
    std::string convKey;    // ConversationKey::toString()（产生该摘要的会话）
    std::string personId;   // 归属人 internalId（群聊记忆可为空：按 conv_key 共享）
    bool isPublic = false;  // 【旧字段】仅保留作迁移证据；权限判断只看 visibility
    SummaryKind kind = SummaryKind::EpisodicMemory;
    Visibility visibility = Visibility::Conversation;
    std::int64_t createdAt = 0;
    std::int64_t eventTime = 0;       // 事件发生时间（0 = 未知，回退 created_at）
    std::int64_t fromMessageId = 0;
    std::int64_t toMessageId = 0;
    std::string source;               // silence_summary / context_compaction / model_tool / legacy_import
    std::vector<std::string> participants;
    std::vector<std::string> evidenceMessageIds;  // 形如 "convKey#messageId"
    std::string summary;              // 摘要正文（召回后注入 prompt 的就是它）
    std::string idempotencyKey;       // manual 记忆的独立幂等键
    EmbeddingStatus embeddingStatus = EmbeddingStatus::Pending;
    std::string migrationNote;        // 迁移记录（如 "legacy is_public narrowed"）
};

// 候选集条目：记录 + 解码后的向量（BLOB → float[]）
struct MemoryCandidate {
    MemoryRecord record;
    std::vector<float> embedding;
};

// 权限过滤条件（SQL 层）：
//   * conversation 可见：conv_key 在允许的会话集合内（默认仅当前会话）；
//   * person 可见：person_id = viewer 且 maxVisibility >= Person；
//   * public 可见：maxVisibility >= Public（AccessContext::canSee 语义）；
//   * 默认排除 context_compaction；只有显式 allowContextCompaction 才召回。
struct MemoryFilter {
    std::string viewerPersonId;
    std::string convKey;
    Visibility maxVisibility = Visibility::Conversation;
    bool allowCrossConversation = false;
    std::vector<std::string> allowedConversationKeys;
    bool allowContextCompaction = false;
    std::size_t limit = 256;  // SQL 预过滤候选上限（新→旧）
};

// 向量化待办
struct EmbeddingTodo {
    std::int64_t id = 0;
    std::int64_t memoryId = 0;
    std::int64_t createdAt = 0;
    int attempts = 0;
    std::string lastError;
};

// 提案待投递（摘要产出 → C 的 ProposalStore）
struct ProposalOutboxItem {
    std::int64_t id = 0;
    std::int64_t memoryId = 0;
    std::int64_t createdAt = 0;
    Proposal proposal;
};

// 迁移审计记录（旧库迁移必须可观察、可重试）
struct MigrationAuditEntry {
    std::int64_t at = 0;
    int schemaVersion = 0;
    std::string action;
    std::string detail;
};

// writeSummaryTx 的结果（存储层语义；MemoryManager 再翻译成 SummaryWriteResult）
struct StoreWriteResult {
    bool ok = false;
    bool duplicate = false;   // 幂等命中（返回原记录 ID）
    std::int64_t memoryId = 0;
    ErrorCode code = ErrorCode::StorageUnavailable;
    std::string message;
};

// Proposal ↔ JSON（proposal_outbox payload；字段与契约一一对应）
nlohmann::json proposalToJson(const Proposal& p);
std::optional<Proposal> proposalFromJson(const nlohmann::json& j);

class MemoryStore {
public:
    // 当前 schema 版本（PRAGMA user_version）。1 = 旧 memories 单表结构。
    static constexpr int kSchemaVersion = 2;

    explicit MemoryStore(std::filesystem::path dbFile);
    ~MemoryStore();
    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    // ---- 写入（单事务）----------------------------------------------------
    // record.embeddingStatus 忽略：新写入一律 pending + 建待办（向量化在事务外）。
    // advanceCursor 只允许 episodic（manual/compaction 由 MemoryManager 传 false）。
    StoreWriteResult writeSummaryTx(const MemoryRecord& record, bool advanceCursor,
                                    const std::vector<Proposal>& proposals,
                                    std::int64_t now);
    // 向量化成功：同事务更新 embedding_status='ok' + 清待办
    bool attachEmbedding(std::int64_t memoryId, const float* vec, std::size_t dim,
                         std::int64_t now);
    // 向量化失败：状态 failed + 待办保留（attempts+1、last_error），不回滚正文
    bool markEmbeddingFailed(std::int64_t memoryId, const std::string& error,
                             std::int64_t now);
    // 没有值得保留的内容：推进游标 + 写摘要审计标记，不产生记忆
    bool markRangeProcessed(const std::string& convKey, std::int64_t toMessageId,
                            const std::string& reason, std::int64_t now);

    // ---- 读取 -------------------------------------------------------------
    std::optional<MemoryRecord> get(std::int64_t memoryId);
    std::vector<MemoryRecord> list(const std::string& convKey, std::size_t limit,
                                   bool includeContextCompaction);
    std::int64_t findRangeRecordId(const std::string& convKey,
                                   std::int64_t fromMessageId,
                                   std::int64_t toMessageId, SummaryKind kind);
    std::int64_t findByIdempotencyKey(const std::string& key);
    // 游标已覆盖该范围时，返回贡献该游标的最近一条经历记忆 ID（可能为 0）
    std::int64_t newestEpisodicIdAtOrBefore(const std::string& convKey,
                                            std::int64_t toMessageId);
    std::int64_t committedCursor(const std::string& convKey);

    std::vector<MemoryCandidate> selectCandidates(const MemoryFilter& filter);
    std::size_t count();

    // ---- 向量化待办 -------------------------------------------------------
    std::vector<EmbeddingTodo> pendingEmbeddingTodos(std::size_t limit);
    std::size_t pendingEmbeddingTodoCount();
    std::size_t failedEmbeddingCount();
    // 清理悬空待办（记录已不存在）：避免每轮重试都重复处理同一行
    bool clearEmbeddingTodo(std::int64_t memoryId);

    // ---- 提案待投递 -------------------------------------------------------
    std::vector<ProposalOutboxItem> pendingProposals(std::size_t limit);
    std::size_t pendingProposalCount();
    bool markProposalDelivered(std::int64_t outboxId, std::int64_t now);
    bool noteProposalDeliveryError(std::int64_t outboxId, const std::string& error);

    // ---- 审计/迁移观测（测试与运维用）-------------------------------------
    int schemaVersion();
    std::vector<MigrationAuditEntry> migrationAudit();
    // 摘要处理审计（markRangeProcessed 等；不含摘要正文）
    struct SummaryAuditEntry {
        std::int64_t at = 0;
        std::string convKey;
        std::string action;
        std::string detail;
        std::int64_t toMessageId = 0;
    };
    std::vector<SummaryAuditEntry> summaryAudit(const std::string& convKey,
                                                std::size_t limit);

private:
    void exec(const char* sql);  // DDL/PRAGMA 快捷方式（必须成功，否则抛出）
    bool tableExists(const char* table);
    bool hasColumn(const char* table, const char* column);
    void migrateLocked();  // 幂等迁移（调用方已持锁）
    void createSchemaLocked();
    void createIndexesLocked();

    // 以下 *Locked 版本假定调用方已持 mtx_（std::mutex 不可重入，
    // 事务内的读写必须走这些版本，否则会自锁死）
    std::int64_t committedCursorLocked(const std::string& convKey);
    std::int64_t findRangeRecordIdLocked(const std::string& convKey,
                                         std::int64_t fromMessageId,
                                         std::int64_t toMessageId, SummaryKind kind);
    std::int64_t findByIdempotencyKeyLocked(const std::string& key);
    std::int64_t newestEpisodicIdAtOrBeforeLocked(const std::string& convKey,
                                                  std::int64_t toMessageId);

    std::filesystem::path dbFile_;
    ::sqlite3* db_ = nullptr;
    std::mutex mtx_;
};

} // namespace mio
