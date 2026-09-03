// MemoryStore 实现（见 memory/MemoryStore.h）
//
// sqlite3 C API 用法备忘：
//   * sqlite3_open_v2 + sqlite3_prepare_v2 + sqlite3_bind_* + sqlite3_step；
//   * 所有句柄非线程安全对象级共享，统一在 mtx_ 下串行使用；
//   * PRAGMA：WAL（读写并发）+ NORMAL（掉电最多丢最后一笔，不损库）+
//     busy_timeout（多进程访问 data/ 目录时排队而非报错）。
//   * BLOB 用 sqlite3_column_blob + sqlite3_column_bytes 原样取出，
//     memcpy 回 float[] —— 长度不对（维度变更/损坏）的行直接丢弃。

#include "memory/store/MemoryStore.h"

#include <sqlite3.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace mio {

namespace {

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

} // namespace

MemoryStore::MemoryStore(std::filesystem::path dbFile)
    : dbFile_(std::move(dbFile)) {
    const int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(dbFile_.string().c_str(), &db_, flags, nullptr) !=
        SQLITE_OK) {
        const std::string msg = db_ != nullptr ? sqlite3_errmsg(db_)
                                               : "sqlite3_open_v2 失败";
        if (db_ != nullptr) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("无法打开记忆库 " + dbFile_.string() + ": " + msg);
    }

    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA busy_timeout=5000");
    exec("CREATE TABLE IF NOT EXISTS memories ("
         "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  conv_key   TEXT    NOT NULL,"
         "  person_id  TEXT    NOT NULL DEFAULT '',"
         "  is_public  INTEGER NOT NULL DEFAULT 0,"
         "  created_at INTEGER NOT NULL,"
         "  summary    TEXT    NOT NULL,"
         "  dim        INTEGER NOT NULL,"
         "  embedding  BLOB    NOT NULL,"
         "  UNIQUE(conv_key, summary))");
    // 权限预过滤的两个支撑索引：公开∪本人 走 (person_id, is_public)，
    // 本会话 走 (conv_key)；配合 LIMIT 把候选集压到几十~几百条
    exec("CREATE INDEX IF NOT EXISTS idx_memories_person "
         "ON memories(person_id, is_public)");
    exec("CREATE INDEX IF NOT EXISTS idx_memories_conv ON memories(conv_key)");
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

bool MemoryStore::insert(const MemoryRecord& record, const float* vec,
                         std::size_t dim) {
    std::lock_guard<std::mutex> lock(mtx_);
    static const char* kSql =
        "INSERT OR IGNORE INTO memories"
        " (conv_key, person_id, is_public, created_at, summary, dim, embedding)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("记忆库 prepare 失败: ") +
                                 sqlite3_errmsg(db_));

    const int blobBytes = static_cast<int>(dim * sizeof(float));
    sqlite3_bind_text(stmt, 1, record.convKey.c_str(),
                      static_cast<int>(record.convKey.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.personId.c_str(),
                      static_cast<int>(record.personId.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, record.isPublic ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, record.createdAt);
    sqlite3_bind_text(stmt, 5, record.summary.c_str(),
                      static_cast<int>(record.summary.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, static_cast<int>(dim));
    sqlite3_bind_blob(stmt, 7, vec, blobBytes, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    const bool inserted = (rc == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return inserted;
}

std::vector<MemoryCandidate> MemoryStore::selectCandidates(
    const MemoryFilter& filter) {
    std::lock_guard<std::mutex> lock(mtx_);
    static const char* kSql =
        "SELECT id, conv_key, person_id, is_public, created_at, summary,"
        "       embedding, dim"
        " FROM memories"
        " WHERE is_public = 1"
        "    OR (?1 != '' AND person_id = ?1)"
        "    OR conv_key = ?2"
        " ORDER BY created_at DESC"
        " LIMIT ?3";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("记忆库 prepare 失败: ") +
                                 sqlite3_errmsg(db_));

    sqlite3_bind_text(stmt, 1, filter.viewerPersonId.c_str(),
                      static_cast<int>(filter.viewerPersonId.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, filter.convKey.c_str(),
                      static_cast<int>(filter.convKey.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<std::int64_t>(filter.limit));

    std::vector<MemoryCandidate> out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MemoryCandidate cand;
        cand.record.id = sqlite3_column_int64(stmt, 0);
        cand.record.convKey =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        cand.record.personId =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        cand.record.isPublic = sqlite3_column_int(stmt, 3) != 0;
        cand.record.createdAt = sqlite3_column_int64(stmt, 4);
        cand.record.summary =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)) != nullptr
                ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))
                : "";
        cand.embedding =
            blobToVector(sqlite3_column_blob(stmt, 6), sqlite3_column_bytes(stmt, 6));
        // 维度对不上（历史数据/损坏行）不进候选集，避免打分时越界
        if (cand.embedding.size() ==
            static_cast<std::size_t>(sqlite3_column_int(stmt, 7)))
            out.push_back(std::move(cand));
    }
    sqlite3_finalize(stmt);
    return out;
}

std::size_t MemoryStore::count() {
    std::lock_guard<std::mutex> lock(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM memories", -1, &stmt,
                           nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("记忆库 prepare 失败: ") +
                                 sqlite3_errmsg(db_));
    std::size_t n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return n;
}

} // namespace mio
