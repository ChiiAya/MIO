#pragma once
// ============================================================================
// MemoryStore（记忆仓库）：SQLite 原生 BLOB 向量存储 + SQL 元数据预过滤
//
// 职责边界（只存/取，不算分）：
//   * 表结构：1024 维 float[] 原生序列化为 4096 字节 BLOB（native endian，
//     本机库本机读，不做跨端交换），随摘要文本与权限元数据一行落盘；
//   * 写入：INSERT OR IGNORE + UNIQUE(conv_key, summary) —— 重启后冷读取
//     重放同一摘要时天然幂等，不产生重复记忆；
//   * 读取：selectCandidates 用 (person_id, is_public) 与 (conv_key) 两个
//     索引在 SQL 层完成权限裁剪（公开 ∪ 本人 ∪ 本会话），配合
//     ORDER BY created_at DESC LIMIT 把候选集压到几十~几百条，
//     向量打分交给 MemoryManager（C++ 内存点积 + 时间衰减）。
//
// 线程安全：内部互斥；sqlite3 连接仅在本对象内使用。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;  // ::sqlite3 前置声明（完整定义在 <sqlite3.h>，仅实现可见）

namespace mio {

// 一条已落库的记忆（不含向量；给展示/工具层用）
struct MemoryRecord {
    std::int64_t id = 0;
    std::string convKey;    // ConversationKey::toString()（产生该摘要的会话）
    std::string personId;   // 归属人 internalId（群聊记忆为空：按 conv_key 共享）
    bool isPublic = false;  // 摘要私密性判定取反（true = 可对所有人召回）
    std::int64_t createdAt = 0;
    std::string summary;    // 摘要正文（召回后注入 prompt 的就是它）
};

// 候选集条目：记录 + 解码后的向量（BLOB → float[]）
struct MemoryCandidate {
    MemoryRecord record;
    std::vector<float> embedding;
};

// 权限过滤条件：候选 = is_public=1 ∪ person_id=viewer ∪ conv_key=conv
// （viewer 为空串时跳过"本人"子句——群聊场景没有单一归属人）
struct MemoryFilter {
    std::string viewerPersonId;
    std::string convKey;
    std::size_t limit = 256;  // SQL 预过滤候选上限（新→旧）
};

class MemoryStore {
public:
    explicit MemoryStore(std::filesystem::path dbFile);
    ~MemoryStore();
    MemoryStore(const MemoryStore&) = delete;
    MemoryStore& operator=(const MemoryStore&) = delete;

    // 写入一条记忆（向量已归一化、维度已校验，由 MemoryManager 保证）。
    // 返回是否真正插入（同 conv_key + 同摘要文本已存在时忽略，返回 false）。
    bool insert(const MemoryRecord& record, const float* vec, std::size_t dim);

    // SQL 权限预过滤：按 created_at 新→旧拉取候选 BLOB 并解码为 float[]
    std::vector<MemoryCandidate> selectCandidates(const MemoryFilter& filter);

    std::size_t count();

private:
    void exec(const char* sql);  // DDL/PRAGMA 快捷方式（必须成功，否则抛出）

    std::filesystem::path dbFile_;
    ::sqlite3* db_ = nullptr;
    std::mutex mtx_;
};

} // namespace mio
