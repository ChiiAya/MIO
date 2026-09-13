// MemoryStore 实现（见 memory/store/MemoryStore.h）
//
// sqlite3 C API 用法备忘：
//   * sqlite3_open_v2 + prepare + bind_* + step；所有句柄在 mtx_ 下串行使用；
//   * PRAGMA：WAL（读写并发）+ NORMAL（掉电最多丢最后一笔，不损库）+
//     busy_timeout（多进程访问 data/ 目录时排队而非报错）；
//   * BLOB 用 sqlite3_column_blob + sqlite3_column_bytes 原样取出，
//     memcpy 回 float[] —— 长度不对（维度变更/损坏）的行直接丢弃；
//   * 迁移：普通 CREATE/ALTER/DROP 都可在事务内执行，整段迁移包在
//     BEGIN IMMEDIATE..COMMIT 里，失败自动 ROLLBACK（旧表保持原样，可重试）。

#include "memory/store/MemoryStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace mio {

namespace {

std::int64_t nowSeconds() { return static_cast<std::int64_t>(std::time(nullptr)); }

// BLOB ↔ float[]：native endian 原生序列化（本机写本机读，不跨端交换）
std::vector<float> blobToVector(const void* data, int bytes) {
    std::vector<float> out;
    if (data == nullptr || bytes <= 0 ||
        static_cast<std::size_t>(bytes) % sizeof(float) != 0)
        return out;
    out.resize(static_cast<std::size_t>(bytes) / sizeof(float));
    std::memcpy(out.data(), data, static_cast<std::size_t>(bytes));
    return out;
}

std::string colText(sqlite3_stmt* stmt, int idx) {
    const unsigned char* p = sqlite3_column_text(stmt, idx);
    if (p == nullptr) return "";
    return reinterpret_cast<const char*>(p);
}

// RAII statement
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) {
            const std::string msg = sqlite3_errmsg(db);
            throw std::runtime_error("记忆库 prepare 失败: " + msg + " | " + sql);
        }
    }
    ~Stmt() {
        if (s_ != nullptr) sqlite3_finalize(s_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    sqlite3_stmt* get() const { return s_; }
    void text(int i, const std::string& v) {
        sqlite3_bind_text(s_, i, v.c_str(), static_cast<int>(v.size()),
                          SQLITE_TRANSIENT);
    }
    void i64(int i, std::int64_t v) { sqlite3_bind_int64(s_, i, v); }
    void i32(int i, int v) { sqlite3_bind_int(s_, i, v); }
    void blob(int i, const float* vec, std::size_t dim) {
        sqlite3_bind_blob(s_, i, vec, static_cast<int>(dim * sizeof(float)),
                          SQLITE_TRANSIENT);
    }
    int step() { return sqlite3_step(s_); }
    bool row() { return sqlite3_step(s_) == SQLITE_ROW; }
    std::int64_t columnI64(int i) { return sqlite3_column_int64(s_, i); }
    int columnInt(int i) { return sqlite3_column_int(s_, i); }
    std::string columnText(int i) { return colText(s_, i); }

private:
    sqlite3* db_;
    sqlite3_stmt* s_ = nullptr;
};

// RAII 事务：析构未 commit 则 ROLLBACK（异常路径不留下半截写入）
class Tx {
public:
    explicit Tx(sqlite3* db) : db_(db) {
        char* err = nullptr;
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string msg = err != nullptr ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("记忆库 BEGIN 失败: " + msg);
        }
    }
    ~Tx() { rollback(); }
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;

    void commit() {
        if (done_) return;
        execOrThrow("COMMIT");
        done_ = true;
    }
    void rollback() {
        if (done_) return;
        char* err = nullptr;
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &err);
        if (err != nullptr) sqlite3_free(err);
        done_ = true;
    }

private:
    void execOrThrow(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string msg = err != nullptr ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error(std::string("记忆库事务失败: ") + msg);
        }
    }
    sqlite3* db_;
    bool done_ = false;
};

std::string joinJsonArray(const std::vector<std::string>& items) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : items) arr.push_back(s);
    return arr.dump();
}

std::vector<std::string> parseJsonArray(const std::string& text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) return out;
    for (const auto& item : j) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

} // namespace

nlohmann::json proposalToJson(const Proposal& p) {
    nlohmann::json j;
    j["proposal_id"] = p.proposalId;
    j["kind"] = toString(p.kind);
    j["subject_id"] = p.subjectId;
    j["predicate"] = p.predicate;
    j["object"] = p.object;
    j["source"] = p.source;
    j["evidence_message_ids"] = p.evidenceMessageIds;
    j["confidence"] = p.confidence;
    j["status"] = toString(p.status);
    j["fact_status"] = toString(p.factStatus);
    j["cognition_status"] = toString(p.cognitionStatus);
    j["review"] = {{"status", p.review.status},
                   {"actor", p.review.actor},
                   {"reason", p.review.reason},
                   {"reviewed_at", p.review.reviewedAt}};
    j["conversation_key"] = p.conversationKey;
    j["visibility"] = toString(p.visibility);
    j["created_at"] = p.createdAt;
    j["valid_from"] = p.validFrom;
    j["valid_to"] = p.validTo;
    j["approved_by"] = p.approvedBy;
    j["claimed_status"] = p.claimedStatus;
    return j;
}

std::optional<Proposal> proposalFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    Proposal p;
    p.proposalId = j.value("proposal_id", "");
    ProposalKind kind = ProposalKind::Fact;
    if (!parseProposalKind(j.value("kind", std::string("fact")), kind)) return std::nullopt;
    p.kind = kind;
    p.subjectId = j.value("subject_id", "");
    p.predicate = j.value("predicate", "");
    p.object = j.value("object", "");
    p.source = j.value("source", "");
    if (auto it = j.find("evidence_message_ids");
        it != j.end() && it->is_array()) {
        for (const auto& e : *it)
            if (e.is_string()) p.evidenceMessageIds.push_back(e.get<std::string>());
    }
    p.confidence = j.value("confidence", 0.0);
    const std::string status = j.value("status", std::string("pending"));
    if (status == "accepted") p.status = ProposalStatus::Accepted;
    else if (status == "rejected") p.status = ProposalStatus::Rejected;
    else if (status == "superseded") p.status = ProposalStatus::Superseded;
    else p.status = ProposalStatus::Pending;
    const std::string factStatus = j.value("fact_status", std::string("proposed"));
    if (factStatus == "confirmed") p.factStatus = FactStatus::Confirmed;
    else if (factStatus == "rejected") p.factStatus = FactStatus::Rejected;
    else if (factStatus == "superseded") p.factStatus = FactStatus::Superseded;
    else p.factStatus = FactStatus::Proposed;
    const std::string cog = j.value("cognition_status", std::string("observed"));
    if (cog == "confirmed") p.cognitionStatus = CognitionStatus::Confirmed;
    else if (cog == "observed") p.cognitionStatus = CognitionStatus::Observed;
    else p.cognitionStatus = CognitionStatus::Inferred;
    if (auto it = j.find("review"); it != j.end() && it->is_object()) {
        p.review.status = it->value("status", "");
        p.review.actor = it->value("actor", "");
        p.review.reason = it->value("reason", "");
        p.review.reviewedAt = it->value("reviewed_at", static_cast<std::int64_t>(0));
    }
    p.conversationKey = j.value("conversation_key", "");
    Visibility vis = Visibility::Conversation;
    parseVisibility(j.value("visibility", std::string("conversation")), vis);
    p.visibility = vis;  // 解析失败保持最窄
    p.createdAt = j.value("created_at", static_cast<std::int64_t>(0));
    p.validFrom = j.value("valid_from", static_cast<std::int64_t>(0));
    p.validTo = j.value("valid_to", static_cast<std::int64_t>(0));
    p.approvedBy = j.value("approved_by", "");
    p.claimedStatus = j.value("claimed_status", "");
    return p;
}

// ---------------------------------------------------------------------------
// 构造 / 迁移
// ---------------------------------------------------------------------------

MemoryStore::MemoryStore(std::filesystem::path dbFile) : dbFile_(std::move(dbFile)) {
    const int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(dbFile_.string().c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        const std::string msg = db_ != nullptr ? sqlite3_errmsg(db_)
                                               : "sqlite3_open_v2 失败";
        if (db_ != nullptr) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("无法打开记忆库 " + dbFile_.string() + ": " + msg);
    }
    try {
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=NORMAL");
        exec("PRAGMA busy_timeout=5000");
        std::lock_guard<std::mutex> lock(mtx_);
        migrateLocked();
    } catch (...) {
        // 构造失败时不进入析构（对象未建成），必须在这里关闭句柄
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

MemoryStore::~MemoryStore() {
    if (db_ != nullptr) sqlite3_close(db_);
}

void MemoryStore::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err != nullptr ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("记忆库 SQL 执行失败: " + msg + " | " + sql);
    }
}

bool MemoryStore::tableExists(const char* table) {
    Stmt stmt(db_,
              "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?1");
    stmt.text(1, table);
    return stmt.row() && stmt.columnI64(0) > 0;
}

bool MemoryStore::hasColumn(const char* table, const char* column) {
    Stmt stmt(db_, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while (stmt.row()) {
        if (stmt.columnText(1) == column) return true;
    }
    return false;
}

int MemoryStore::schemaVersion() {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_, "PRAGMA user_version");
    return stmt.row() ? stmt.columnInt(0) : 0;
}

void MemoryStore::createSchemaLocked() {
    exec("CREATE TABLE IF NOT EXISTS memories ("
         "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  conv_key          TEXT    NOT NULL,"
         "  person_id         TEXT    NOT NULL DEFAULT '',"
         "  is_public         INTEGER NOT NULL DEFAULT 0,"
         "  created_at        INTEGER NOT NULL,"
         "  summary           TEXT    NOT NULL,"
         "  dim               INTEGER NOT NULL DEFAULT 0,"
         "  embedding         BLOB    NOT NULL DEFAULT x'',"
         "  summary_kind      TEXT    NOT NULL DEFAULT 'episodic_memory',"
         "  from_message_id   INTEGER NOT NULL DEFAULT 0,"
         "  to_message_id     INTEGER NOT NULL DEFAULT 0,"
         "  source            TEXT    NOT NULL DEFAULT '',"
         "  visibility        TEXT    NOT NULL DEFAULT 'conversation',"
         "  participants      TEXT    NOT NULL DEFAULT '[]',"
         "  embedding_status  TEXT    NOT NULL DEFAULT 'pending',"
         "  idempotency_key   TEXT    NOT NULL DEFAULT '',"
         "  event_time        INTEGER NOT NULL DEFAULT 0,"
         "  evidence_ids      TEXT    NOT NULL DEFAULT '[]',"
         "  migration_note    TEXT    NOT NULL DEFAULT '')");
    exec("CREATE TABLE IF NOT EXISTS summary_cursor ("
         "  conv_key        TEXT PRIMARY KEY,"
         "  last_message_id INTEGER NOT NULL,"
         "  updated_at      INTEGER NOT NULL,"
         "  schema_version  INTEGER NOT NULL DEFAULT 2)");
    exec("CREATE TABLE IF NOT EXISTS embedding_todo ("
         "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  memory_id  INTEGER NOT NULL UNIQUE,"
         "  created_at INTEGER NOT NULL,"
         "  attempts   INTEGER NOT NULL DEFAULT 0,"
         "  last_error TEXT    NOT NULL DEFAULT '')");
    exec("CREATE TABLE IF NOT EXISTS proposal_outbox ("
         "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  memory_id    INTEGER NOT NULL,"
         "  payload      TEXT    NOT NULL,"
         "  created_at   INTEGER NOT NULL,"
         "  delivered    INTEGER NOT NULL DEFAULT 0,"
         "  delivered_at INTEGER NOT NULL DEFAULT 0,"
         "  last_error   TEXT    NOT NULL DEFAULT '')");
    exec("CREATE TABLE IF NOT EXISTS migration_audit ("
         "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  at             INTEGER NOT NULL,"
         "  schema_version INTEGER NOT NULL,"
         "  action         TEXT    NOT NULL,"
         "  detail         TEXT    NOT NULL)");
    exec("CREATE TABLE IF NOT EXISTS summary_audit ("
         "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  at             INTEGER NOT NULL,"
         "  conv_key       TEXT    NOT NULL,"
         "  action         TEXT    NOT NULL,"
         "  detail         TEXT    NOT NULL,"
         "  to_message_id  INTEGER NOT NULL DEFAULT 0,"
         "  schema_version INTEGER NOT NULL DEFAULT 2)");
}

void MemoryStore::createIndexesLocked() {
    // 范围幂等：只用部分唯一索引，避开旧行（from/to = 0）与 manual（走 idempotency_key）
    exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_memories_range"
         " ON memories(conv_key, from_message_id, to_message_id, summary_kind)"
         " WHERE from_message_id > 0 AND summary_kind <> 'manual'");
    exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_memories_idem"
         " ON memories(idempotency_key) WHERE idempotency_key <> ''");
    exec("CREATE INDEX IF NOT EXISTS idx_memories_person"
         " ON memories(person_id, visibility)");
    exec("CREATE INDEX IF NOT EXISTS idx_memories_conv"
         " ON memories(conv_key, created_at)");
    exec("CREATE INDEX IF NOT EXISTS idx_memories_visibility"
         " ON memories(visibility, created_at)");
    exec("CREATE INDEX IF NOT EXISTS idx_todo_memory ON embedding_todo(memory_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_outbox_pending"
         " ON proposal_outbox(delivered, id)");
    exec("CREATE INDEX IF NOT EXISTS idx_summary_audit_conv"
         " ON summary_audit(conv_key, id)");
}

void MemoryStore::migrateLocked() {
    const bool hadMemories = tableExists("memories");
    const bool legacyShape = hadMemories && !hasColumn("memories", "summary_kind");
    const bool missingTables = !tableExists("summary_cursor") ||
                               !tableExists("embedding_todo") ||
                               !tableExists("proposal_outbox") ||
                               !tableExists("migration_audit") ||
                               !tableExists("summary_audit");
    int version = 0;
    {
        // 语句必须在进入事务前 finalize：活动中的 PRAGMA 语句会持有 schema 读锁，
        // 导致事务里的 ALTER/DROP TABLE 报 "database table is locked"。
        Stmt versionStmt(db_, "PRAGMA user_version");
        if (versionStmt.row()) version = versionStmt.columnInt(0);
    }
    const bool needsWork =
        !hadMemories || legacyShape || missingTables || version != kSchemaVersion;
    if (!needsWork) return;

    const std::int64_t now = nowSeconds();
    Tx tx(db_);

    if (legacyShape) {
        // 旧表改名保留，按新 schema 建表后整表搬运（id 不变）：
        //   * 旧行的 is_public 原值保留为迁移证据，但权威 visibility 收窄为
        //     conversation（未确认来源前不得公开）；
        //   * 已具备完整向量的旧行标 embedding_status='ok'（无需重新向量化），
        //     其余标 pending 并补向量化待办；
        //   * source 必须非空 → legacy_import；event_time 回退 created_at。
        if (tableExists("memories_legacy_v1")) {
            throw std::runtime_error(
                "记忆库存在未完成的迁移残留表 memories_legacy_v1，"
                "拒绝自动覆盖；请人工确认后处理（原数据未被删除）");
        }
        exec("ALTER TABLE memories RENAME TO memories_legacy_v1");
        createSchemaLocked();
        exec("INSERT INTO memories"
             " (id, conv_key, person_id, is_public, created_at, summary, dim,"
             "  embedding, summary_kind, from_message_id, to_message_id, source,"
             "  visibility, participants, embedding_status, idempotency_key,"
             "  event_time, evidence_ids, migration_note)"
             " SELECT id, conv_key, person_id, is_public, created_at, summary, dim,"
             "  embedding, 'episodic_memory', 0, 0, 'legacy_import',"
             "  'conversation', '[]',"
             "  CASE WHEN dim > 0 AND length(embedding) = dim * 4 THEN 'ok'"
             "       ELSE 'pending' END,"
             "  '', created_at, '[]',"
             "  CASE WHEN is_public = 1 THEN 'legacy is_public narrowed'"
             "       ELSE 'legacy_import' END"
             " FROM memories_legacy_v1");
        exec("DROP TABLE memories_legacy_v1");
        // 补向量化待办（仅未就绪的行；UNIQUE(memory_id) 保证可重试幂等）
        {
            Stmt stmt(db_,
                      "INSERT OR IGNORE INTO embedding_todo"
                      " (memory_id, created_at, attempts, last_error)"
                      " SELECT id, ?1, 0, 'legacy_migration' FROM memories"
                      " WHERE embedding_status <> 'ok'");
            stmt.i64(1, now);
            stmt.step();
        }
    }

    createSchemaLocked();
    createIndexesLocked();
    exec(("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());

    {
        Stmt stmt(db_,
                  "INSERT INTO migration_audit"
                  " (at, schema_version, action, detail) VALUES (?1, ?2, ?3, ?4)");
        stmt.i64(1, now);
        stmt.i32(2, kSchemaVersion);
        if (legacyShape) {
            stmt.text(3, "migrate_v1_to_v2");
            stmt.text(4,
                      "legacy is_public narrowed to conversation; old rows preserved "
                      "(is_public kept as migration evidence); text not dropped");
        } else {
            stmt.text(3, "init_schema");
            stmt.text(4, "schema created/verified; no legacy rows");
        }
        stmt.step();
    }
    tx.commit();
}

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------

StoreWriteResult MemoryStore::writeSummaryTx(const MemoryRecord& record,
                                             bool advanceCursor,
                                             const std::vector<Proposal>& proposals,
                                             std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    StoreWriteResult r;

    // 存储层兜底：强制字段不得为空（绕过 MemoryManager 直接调用也必须被拒绝）
    if (record.convKey.empty() || record.source.empty() ||
        record.fromMessageId <= 0 || record.toMessageId < record.fromMessageId ||
        toString(record.kind)[0] == '\0') {
        r.ok = false;
        r.code = ErrorCode::InvalidArgument;
        r.message = "摘要记录缺少强制字段（conv_key/source/from/to/summary_kind）";
        return r;
    }

    auto duplicateResult = [&](std::int64_t id, const std::string& why) {
        r.ok = true;
        r.duplicate = true;
        r.memoryId = id;
        r.code = ErrorCode::Ok;
        r.message = why;
        return r;
    };

    // ---- 幂等预检（事务外读一次；事务内仍由唯一索引兜底）----
    if (record.kind == SummaryKind::Manual) {
        if (!record.idempotencyKey.empty()) {
            const std::int64_t id = findByIdempotencyKeyLocked(record.idempotencyKey);
            if (id != 0) return duplicateResult(id, "manual 幂等键命中：返回原记录");
        }
    } else {
        const std::int64_t id = findRangeRecordIdLocked(
            record.convKey, record.fromMessageId, record.toMessageId, record.kind);
        if (id != 0) return duplicateResult(id, "相同消息范围已存在：返回原记录");
        // 提交时比较旧游标：episodic 且 to <= 已提交游标 → 视为已提交，防止
        // 重叠范围重复提交（返回贡献该游标的最近记录 ID，可能为 0）。
        if (record.kind == SummaryKind::EpisodicMemory && record.toMessageId > 0 &&
            record.toMessageId <= committedCursorLocked(record.convKey)) {
            return duplicateResult(
                newestEpisodicIdAtOrBeforeLocked(record.convKey, record.toMessageId),
                "消息范围已被摘要游标覆盖：不重复写入");
        }
    }

    try {
        Tx tx(db_);
        // 1) 摘要行
        {
            Stmt stmt(
                db_,
                "INSERT INTO memories"
                " (conv_key, person_id, is_public, created_at, summary, dim, embedding,"
                "  summary_kind, from_message_id, to_message_id, source, visibility,"
                "  participants, embedding_status, idempotency_key, event_time,"
                "  evidence_ids, migration_note)"
                " VALUES (?1, ?2, ?3, ?4, ?5, 0, x'', ?6, ?7, ?8, ?9, ?10, ?11,"
                "         'pending', ?12, ?13, ?14, '')");
            stmt.text(1, record.convKey);
            stmt.text(2, record.personId);
            stmt.i32(3, record.isPublic ? 1 : 0);
            stmt.i64(4, record.createdAt > 0 ? record.createdAt : now);
            stmt.text(5, record.summary);
            stmt.text(6, toString(record.kind));
            stmt.i64(7, record.fromMessageId);
            stmt.i64(8, record.toMessageId);
            stmt.text(9, record.source);
            stmt.text(10, toString(record.visibility));
            stmt.text(11, joinJsonArray(record.participants));
            stmt.text(12, record.idempotencyKey);
            stmt.i64(13, record.eventTime > 0 ? record.eventTime
                                              : (record.createdAt > 0 ? record.createdAt
                                                                      : now));
            stmt.text(14, joinJsonArray(record.evidenceMessageIds));
            const int rc = stmt.step();
            if (rc == SQLITE_CONSTRAINT) {
                // 并发进程抢先插入同一范围/幂等键：回滚后按幂等处理
                tx.rollback();
                std::int64_t id = 0;
                if (record.kind == SummaryKind::Manual && !record.idempotencyKey.empty())
                    id = findByIdempotencyKeyLocked(record.idempotencyKey);
                else
                    id = findRangeRecordIdLocked(record.convKey, record.fromMessageId,
                                                 record.toMessageId, record.kind);
                if (id != 0) return duplicateResult(id, "唯一索引命中：返回原记录");
                r.ok = false;
                r.code = ErrorCode::Conflict;
                r.message = std::string("写入冲突且未找到原记录: ") + sqlite3_errmsg(db_);
                return r;
            }
            if (rc != SQLITE_DONE)
                throw std::runtime_error(std::string("摘要行写入失败: ") +
                                         sqlite3_errmsg(db_));
        }
        const std::int64_t memoryId = sqlite3_last_insert_rowid(db_);

        // 2) 向量化待办（本地持久化依据：后端返回 queued 也只以此为准）
        {
            Stmt stmt(db_,
                      "INSERT OR REPLACE INTO embedding_todo"
                      " (memory_id, created_at, attempts, last_error)"
                      " VALUES (?1, ?2, 0, '')");
            stmt.i64(1, memoryId);
            stmt.i64(2, now);
            if (stmt.step() != SQLITE_DONE)
                throw std::runtime_error(std::string("向量化待办写入失败: ") +
                                         sqlite3_errmsg(db_));
        }

        // 3) 摘要游标（只有 episodic 推进）
        if (advanceCursor && record.kind == SummaryKind::EpisodicMemory) {
            const std::int64_t cur = committedCursorLocked(record.convKey);
            if (record.toMessageId > cur) {
                Stmt stmt(db_,
                          "INSERT OR REPLACE INTO summary_cursor"
                          " (conv_key, last_message_id, updated_at, schema_version)"
                          " VALUES (?1, ?2, ?3, ?4)");
                stmt.text(1, record.convKey);
                stmt.i64(2, record.toMessageId);
                stmt.i64(3, now);
                stmt.i32(4, kSchemaVersion);
                if (stmt.step() != SQLITE_DONE)
                    throw std::runtime_error(std::string("摘要游标写入失败: ") +
                                             sqlite3_errmsg(db_));
            }
        }

        // 4) 提案待投递记录（同一事务：崩溃后重启仍能投递给 ProposalStore）
        for (const auto& p : proposals) {
            Stmt stmt(db_,
                      "INSERT INTO proposal_outbox"
                      " (memory_id, payload, created_at, delivered, delivered_at,"
                      "  last_error) VALUES (?1, ?2, ?3, 0, 0, '')");
            stmt.i64(1, memoryId);
            stmt.text(2, proposalToJson(p).dump());
            stmt.i64(3, now);
            if (stmt.step() != SQLITE_DONE)
                throw std::runtime_error(std::string("提案待投递写入失败: ") +
                                         sqlite3_errmsg(db_));
        }
        tx.commit();
        r.ok = true;
        r.code = ErrorCode::Ok;
        r.memoryId = memoryId;
    } catch (const std::exception& e) {
        r.ok = false;
        r.code = ErrorCode::StorageUnavailable;
        r.message = e.what();
    }
    return r;
}

bool MemoryStore::attachEmbedding(std::int64_t memoryId, const float* vec,
                                  std::size_t dim, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    Tx tx(db_);
    bool found = false;
    {
        Stmt stmt(db_,
                  "UPDATE memories SET embedding = ?2, dim = ?3,"
                  " embedding_status = 'ok' WHERE id = ?1");
        stmt.i64(1, memoryId);
        stmt.blob(2, vec, dim);
        stmt.i64(3, static_cast<std::int64_t>(dim));
        if (stmt.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("向量写回失败: ") + sqlite3_errmsg(db_));
        found = sqlite3_changes(db_) > 0;
    }
    {
        Stmt stmt(db_, "DELETE FROM embedding_todo WHERE memory_id = ?1");
        stmt.i64(1, memoryId);
        if (stmt.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("待办清理失败: ") + sqlite3_errmsg(db_));
    }
    tx.commit();
    (void)now;
    return found;
}

bool MemoryStore::markEmbeddingFailed(std::int64_t memoryId,
                                      const std::string& error,
                                      std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    Tx tx(db_);
    {
        Stmt stmt(db_,
                  // 不得把已就绪的向量降级为 failed（只更新尚未成功的行）
                  "UPDATE memories SET embedding_status = 'failed'"
                  " WHERE id = ?1 AND embedding_status <> 'ok'");
        stmt.i64(1, memoryId);
        if (stmt.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("向量状态更新失败: ") +
                                     sqlite3_errmsg(db_));
    }
    {
        // 待办必须保留（含 attempts / last_error），失败绝不回滚已保存的摘要文本
        Stmt ins(db_,
                 "INSERT OR IGNORE INTO embedding_todo"
                 " (memory_id, created_at, attempts, last_error)"
                 " VALUES (?1, ?2, 0, '')");
        ins.i64(1, memoryId);
        ins.i64(2, now);
        if (ins.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("待办补建失败: ") + sqlite3_errmsg(db_));
        Stmt upd(db_,
                 "UPDATE embedding_todo SET attempts = attempts + 1,"
                 " last_error = ?2 WHERE memory_id = ?1");
        upd.i64(1, memoryId);
        upd.text(2, error);
        if (upd.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("待办更新失败: ") + sqlite3_errmsg(db_));
    }
    tx.commit();
    return true;
}

bool MemoryStore::markRangeProcessed(const std::string& convKey,
                                     std::int64_t toMessageId,
                                     const std::string& reason, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::int64_t cur = committedCursorLocked(convKey);
    if (toMessageId <= cur) return true;  // 已推进过：幂等，不重复写审计

    Tx tx(db_);
    {
        Stmt stmt(db_,
                  "INSERT OR REPLACE INTO summary_cursor"
                  " (conv_key, last_message_id, updated_at, schema_version)"
                  " VALUES (?1, ?2, ?3, ?4)");
        stmt.text(1, convKey);
        stmt.i64(2, toMessageId);
        stmt.i64(3, now);
        stmt.i32(4, kSchemaVersion);
        if (stmt.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("游标推进失败: ") + sqlite3_errmsg(db_));
    }
    {
        // 可审计的处理标记：没有值得保留的内容也要留下"已处理到哪"的记录
        Stmt stmt(db_,
                  "INSERT INTO summary_audit"
                  " (at, conv_key, action, detail, to_message_id, schema_version)"
                  " VALUES (?1, ?2, 'mark_range_processed', ?3, ?4, ?5)");
        stmt.i64(1, now);
        stmt.text(2, convKey);
        stmt.text(3, reason);
        stmt.i64(4, toMessageId);
        stmt.i32(5, kSchemaVersion);
        if (stmt.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("审计标记写入失败: ") +
                                     sqlite3_errmsg(db_));
    }
    tx.commit();
    return true;
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------

namespace {

MemoryRecord readRecord(sqlite3_stmt* stmt) {
    MemoryRecord r;
    r.id = sqlite3_column_int64(stmt, 0);
    r.convKey = colText(stmt, 1);
    r.personId = colText(stmt, 2);
    r.isPublic = sqlite3_column_int(stmt, 3) != 0;
    r.createdAt = sqlite3_column_int64(stmt, 4);
    r.summary = colText(stmt, 5);
    SummaryKind kind = SummaryKind::EpisodicMemory;
    const std::string kindText = colText(stmt, 6);
    if (!parseSummaryKind(kindText, kind)) kind = SummaryKind::EpisodicMemory;
    r.kind = kind;
    Visibility vis = Visibility::Conversation;
    const std::string visText = colText(stmt, 7);
    if (!parseVisibility(visText, vis)) vis = Visibility::Conversation;  // 未知 → 最窄
    r.visibility = vis;
    r.participants = parseJsonArray(colText(stmt, 8));
    r.fromMessageId = sqlite3_column_int64(stmt, 9);
    r.toMessageId = sqlite3_column_int64(stmt, 10);
    r.source = colText(stmt, 11);
    r.eventTime = sqlite3_column_int64(stmt, 12);
    r.idempotencyKey = colText(stmt, 13);
    EmbeddingStatus es = EmbeddingStatus::Pending;
    const std::string esText = colText(stmt, 14);
    if (!parseEmbeddingStatus(esText, es)) es = EmbeddingStatus::Pending;
    r.embeddingStatus = es;
    r.evidenceMessageIds = parseJsonArray(colText(stmt, 15));
    r.migrationNote = colText(stmt, 16);
    return r;
}

constexpr const char* kSelectRecordColumns =
    "id, conv_key, person_id, is_public, created_at, summary, summary_kind,"
    " visibility, participants, from_message_id, to_message_id, source,"
    " event_time, idempotency_key, embedding_status, evidence_ids, migration_note";

} // namespace

std::optional<MemoryRecord> MemoryStore::get(std::int64_t memoryId) {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_, (std::string("SELECT ") + kSelectRecordColumns +
                    " FROM memories WHERE id = ?1")
                       .c_str());
    stmt.i64(1, memoryId);
    if (!stmt.row()) return std::nullopt;
    return readRecord(stmt.get());
}

std::vector<MemoryRecord> MemoryStore::list(const std::string& convKey,
                                            std::size_t limit,
                                            bool includeContextCompaction) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string sql = std::string("SELECT ") + kSelectRecordColumns +
                      " FROM memories WHERE conv_key = ?1";
    if (!includeContextCompaction) sql += " AND summary_kind <> 'context_compaction'";
    sql += " ORDER BY id DESC LIMIT ?2";
    Stmt stmt(db_, sql.c_str());
    stmt.text(1, convKey);
    stmt.i64(2, static_cast<std::int64_t>(limit));
    std::vector<MemoryRecord> out;
    while (stmt.row()) out.push_back(readRecord(stmt.get()));
    return out;
}

std::int64_t MemoryStore::findRangeRecordId(const std::string& convKey,
                                            std::int64_t fromMessageId,
                                            std::int64_t toMessageId,
                                            SummaryKind kind) {
    std::lock_guard<std::mutex> lock(mtx_);
    return findRangeRecordIdLocked(convKey, fromMessageId, toMessageId, kind);
}

std::int64_t MemoryStore::findRangeRecordIdLocked(const std::string& convKey,
                                                  std::int64_t fromMessageId,
                                                  std::int64_t toMessageId,
                                                  SummaryKind kind) {
    Stmt stmt(db_,
              "SELECT id FROM memories WHERE conv_key = ?1 AND from_message_id = ?2"
              " AND to_message_id = ?3 AND summary_kind = ?4 LIMIT 1");
    stmt.text(1, convKey);
    stmt.i64(2, fromMessageId);
    stmt.i64(3, toMessageId);
    stmt.text(4, toString(kind));
    return stmt.row() ? stmt.columnI64(0) : 0;
}

std::int64_t MemoryStore::findByIdempotencyKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx_);
    return findByIdempotencyKeyLocked(key);
}

std::int64_t MemoryStore::findByIdempotencyKeyLocked(const std::string& key) {
    if (key.empty()) return 0;
    Stmt stmt(db_, "SELECT id FROM memories WHERE idempotency_key = ?1 LIMIT 1");
    stmt.text(1, key);
    return stmt.row() ? stmt.columnI64(0) : 0;
}

std::int64_t MemoryStore::newestEpisodicIdAtOrBefore(const std::string& convKey,
                                                     std::int64_t toMessageId) {
    std::lock_guard<std::mutex> lock(mtx_);
    return newestEpisodicIdAtOrBeforeLocked(convKey, toMessageId);
}

std::int64_t MemoryStore::newestEpisodicIdAtOrBeforeLocked(
    const std::string& convKey, std::int64_t toMessageId) {
    Stmt stmt(db_,
              "SELECT id FROM memories WHERE conv_key = ?1"
              " AND summary_kind = 'episodic_memory' AND to_message_id <= ?2"
              " ORDER BY to_message_id DESC, id DESC LIMIT 1");
    stmt.text(1, convKey);
    stmt.i64(2, toMessageId);
    return stmt.row() ? stmt.columnI64(0) : 0;
}

std::int64_t MemoryStore::committedCursor(const std::string& convKey) {
    std::lock_guard<std::mutex> lock(mtx_);
    return committedCursorLocked(convKey);
}

std::int64_t MemoryStore::committedCursorLocked(const std::string& convKey) {
    Stmt stmt(db_,
              "SELECT last_message_id FROM summary_cursor WHERE conv_key = ?1");
    stmt.text(1, convKey);
    return stmt.row() ? stmt.columnI64(0) : 0;
}

std::vector<MemoryCandidate> MemoryStore::selectCandidates(
    const MemoryFilter& filter) {
    std::lock_guard<std::mutex> lock(mtx_);

    // 可见性子句在 SQL 层完成：不可见的行不进候选集，不做"先取后过滤"
    const bool personAllowed =
        static_cast<int>(filter.maxVisibility) >= static_cast<int>(Visibility::Person) &&
        !filter.viewerPersonId.empty();
    const bool publicAllowed =
        static_cast<int>(filter.maxVisibility) >= static_cast<int>(Visibility::Public);

    std::string sql = std::string("SELECT ") + kSelectRecordColumns +
                      ", embedding, dim FROM memories"
                      " WHERE embedding_status = 'ok' AND dim > 0"
                      " AND length(embedding) = dim * 4";

    // conversation 可见：会话必须在允许集合内（默认仅当前会话）
    std::vector<std::string> convKeys;
    if (!filter.convKey.empty()) convKeys.push_back(filter.convKey);
    if (filter.allowCrossConversation) {
        for (const auto& k : filter.allowedConversationKeys) {
            if (k.empty() ||
                std::find(convKeys.begin(), convKeys.end(), k) != convKeys.end())
                continue;
            convKeys.push_back(k);
        }
    }

    int next = 1;
    std::vector<std::string> textBinds;  // 按占位符顺序；编号 = 下标 + 1
    std::vector<std::string> scopeOr;
    if (!convKeys.empty()) {
        std::string clause = "(visibility = 'conversation' AND conv_key IN (";
        for (std::size_t i = 0; i < convKeys.size(); ++i) {
            if (i > 0) clause += ",";
            clause += "?" + std::to_string(next++);
            textBinds.push_back(convKeys[i]);
        }
        clause += "))";
        scopeOr.push_back(std::move(clause));
    }
    if (personAllowed) {
        scopeOr.push_back("(visibility = 'person' AND person_id = ?" +
                          std::to_string(next++) + ")");
        textBinds.push_back(filter.viewerPersonId);
    }
    if (publicAllowed) scopeOr.push_back("(visibility = 'public')");

    if (scopeOr.empty()) return {};  // 无任何可见范围：不返回候选

    sql += " AND (";
    for (std::size_t i = 0; i < scopeOr.size(); ++i) {
        if (i > 0) sql += " OR ";
        sql += scopeOr[i];
    }
    sql += ")";
    if (!filter.allowContextCompaction)
        sql += " AND summary_kind <> 'context_compaction'";
    const int limitParam = next;
    sql += " ORDER BY created_at DESC LIMIT ?" + std::to_string(limitParam);

    Stmt stmt(db_, sql.c_str());
    for (std::size_t i = 0; i < textBinds.size(); ++i)
        stmt.text(static_cast<int>(i) + 1, textBinds[i]);
    stmt.i64(limitParam, static_cast<std::int64_t>(filter.limit));

    std::vector<MemoryCandidate> out;
    while (stmt.row()) {
        MemoryCandidate cand;
        cand.record = readRecord(stmt.get());
        const int dim = stmt.columnInt(18);
        cand.embedding = blobToVector(sqlite3_column_blob(stmt.get(), 17),
                                      sqlite3_column_bytes(stmt.get(), 17));
        if (cand.embedding.empty() ||
            cand.embedding.size() != static_cast<std::size_t>(dim))
            continue;  // 维度对不上（历史数据/损坏行）不进候选集
        out.push_back(std::move(cand));
    }
    return out;
}

std::size_t MemoryStore::count() {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_, "SELECT COUNT(*) FROM memories");
    return stmt.row() ? static_cast<std::size_t>(stmt.columnI64(0)) : 0;
}

std::vector<EmbeddingTodo> MemoryStore::pendingEmbeddingTodos(std::size_t limit) {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_,
              "SELECT id, memory_id, created_at, attempts, last_error"
              " FROM embedding_todo ORDER BY id ASC LIMIT ?1");
    stmt.i64(1, static_cast<std::int64_t>(limit));
    std::vector<EmbeddingTodo> out;
    while (stmt.row()) {
        EmbeddingTodo t;
        t.id = stmt.columnI64(0);
        t.memoryId = stmt.columnI64(1);
        t.createdAt = stmt.columnI64(2);
        t.attempts = stmt.columnInt(3);
        t.lastError = stmt.columnText(4);
        out.push_back(std::move(t));
    }
    return out;
}

std::size_t MemoryStore::pendingEmbeddingTodoCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_, "SELECT COUNT(*) FROM embedding_todo");
    return stmt.row() ? static_cast<std::size_t>(stmt.columnI64(0)) : 0;
}

std::size_t MemoryStore::failedEmbeddingCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_, "SELECT COUNT(*) FROM memories WHERE embedding_status = 'failed'");
    return stmt.row() ? static_cast<std::size_t>(stmt.columnI64(0)) : 0;
}

bool MemoryStore::clearEmbeddingTodo(std::int64_t memoryId) {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_, "DELETE FROM embedding_todo WHERE memory_id = ?1");
    stmt.i64(1, memoryId);
    if (stmt.step() != SQLITE_DONE)
        throw std::runtime_error(std::string("待办清理失败: ") + sqlite3_errmsg(db_));
    return true;
}

std::vector<ProposalOutboxItem> MemoryStore::pendingProposals(std::size_t limit) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<ProposalOutboxItem> out;
    std::vector<std::int64_t> quarantine;
    {
        Stmt stmt(db_,
                  "SELECT id, memory_id, payload, created_at FROM proposal_outbox"
                  " WHERE delivered = 0 ORDER BY id ASC LIMIT ?1");
        stmt.i64(1, static_cast<std::int64_t>(limit));
        while (stmt.row()) {
            ProposalOutboxItem item;
            item.id = stmt.columnI64(0);
            item.memoryId = stmt.columnI64(1);
            item.createdAt = stmt.columnI64(3);
            const std::string payload = stmt.columnText(2);
            nlohmann::json j = nlohmann::json::parse(payload, nullptr, false);
            auto parsed = j.is_discarded() ? std::nullopt : proposalFromJson(j);
            if (!parsed) {
                quarantine.push_back(item.id);  // 损坏 payload：隔离，避免永久阻塞
                continue;
            }
            item.proposal = std::move(*parsed);
            out.push_back(std::move(item));
        }
    }  // SELECT 语句已 finalize，再写同表
    for (const auto id : quarantine) {
        Stmt q(db_,
               "UPDATE proposal_outbox SET delivered = 1, last_error = ?2"
               " WHERE id = ?1");
        q.i64(1, id);
        q.text(2, "payload 损坏：已隔离，未投递");
        q.step();
    }
    return out;
}

std::size_t MemoryStore::pendingProposalCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_, "SELECT COUNT(*) FROM proposal_outbox WHERE delivered = 0");
    return stmt.row() ? static_cast<std::size_t>(stmt.columnI64(0)) : 0;
}

bool MemoryStore::markProposalDelivered(std::int64_t outboxId, std::int64_t now) {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_,
              "UPDATE proposal_outbox SET delivered = 1, delivered_at = ?2,"
              " last_error = '' WHERE id = ?1");
    stmt.i64(1, outboxId);
    stmt.i64(2, now);
    if (stmt.step() != SQLITE_DONE)
        throw std::runtime_error(std::string("提案投递标记失败: ") +
                                 sqlite3_errmsg(db_));
    return true;
}

bool MemoryStore::noteProposalDeliveryError(std::int64_t outboxId,
                                            const std::string& error) {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_,
              "UPDATE proposal_outbox SET last_error = ?2"
              " WHERE id = ?1 AND delivered = 0");
    stmt.i64(1, outboxId);
    stmt.text(2, error);
    if (stmt.step() != SQLITE_DONE)
        throw std::runtime_error(std::string("提案投递错误记录失败: ") +
                                 sqlite3_errmsg(db_));
    return true;
}

std::vector<MigrationAuditEntry> MemoryStore::migrationAudit() {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_,
              "SELECT at, schema_version, action, detail FROM migration_audit"
              " ORDER BY id ASC");
    std::vector<MigrationAuditEntry> out;
    while (stmt.row()) {
        MigrationAuditEntry e;
        e.at = stmt.columnI64(0);
        e.schemaVersion = stmt.columnInt(1);
        e.action = stmt.columnText(2);
        e.detail = stmt.columnText(3);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<MemoryStore::SummaryAuditEntry> MemoryStore::summaryAudit(
    const std::string& convKey, std::size_t limit) {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt stmt(db_,
              "SELECT at, conv_key, action, detail, to_message_id FROM summary_audit"
              " WHERE conv_key = ?1 ORDER BY id ASC LIMIT ?2");
    stmt.text(1, convKey);
    stmt.i64(2, static_cast<std::int64_t>(limit));
    std::vector<SummaryAuditEntry> out;
    while (stmt.row()) {
        SummaryAuditEntry e;
        e.at = stmt.columnI64(0);
        e.convKey = stmt.columnText(1);
        e.action = stmt.columnText(2);
        e.detail = stmt.columnText(3);
        e.toMessageId = stmt.columnI64(4);
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace mio
