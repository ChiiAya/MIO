#pragma once
// ============================================================================
// MemoryManager（记忆系统编排）：SELECT & RECALL 混合检索的两段式实现
//
// 写入流程（remember）：
//   Summary 文本 ──> Embedding 向量化（BGE-M3 1024 维）
//                ──> L2 归一化 ──> 序列化 4096 字节 BLOB ──> INSERT SQLite
//
// 检索流程（recall）：
//   Query ──> Embedding 向量化 + 归一化
//         ──> SQL 权限预过滤（person_id / is_public / conv_key，
//             命中索引，微秒级裁剪出几十~几百条候选 BLOB）
//         ──> C++ 内存 SIMD 点积（候选量过阈值时多线程切分）
//             + 时间衰减打分 exp(-Δt/τ)
//         ──> Top-K 返回（调用方注入 prompt）
//
// 权限隔离（v1）：一条记忆可见 ⇔ 公开(is_public) ∪ 本人(person_id) ∪
// 本会话(conv_key)。私密摘要只对归属人（跨会话）与其产生会话可见，
// 群聊记忆按 conv_key 共享 —— 归属判定在 Runtime 组装层完成。
//
// 降级承诺：embedding 端点不可用时 remember 静默丢弃、recall 返回空，
// 绝不向聊天主链路抛异常；连续失败进入 60s 冷却，不反复敲死端点。
// ============================================================================

#include "core/conversation/Conversation.h"
#include "providers/embedding/Embedding.h"
#include "memory/store/MemoryStore.h"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace mio {

struct MemoryConfig {
    std::size_t dim = 1024;                     // BGE-M3 维度（BLOB = dim*4 字节）
    std::size_t maxCandidates = 256;            // SQL 预过滤候选上限
    std::size_t topK = 3;                       // 注入 prompt 的记忆条数
    double minSimilarity = 0.30;                // 余弦门槛（低于丢弃，防垃圾注入）
    double decayTauSeconds = 30.0 * 24 * 3600;  // 时间衰减 τ（≈半月衰到 0.37）
};

struct RecalledMemory {
    std::int64_t id = 0;
    std::string convKey;
    std::string personId;
    bool isPublic = false;
    std::int64_t createdAt = 0;
    std::string summary;
    double similarity = 0;  // 余弦相似度（0~1）
    double score = 0;       // similarity × 时间衰减，Top-K 依据
};

class MemoryManager {
public:
    MemoryManager(MemoryConfig cfg, std::shared_ptr<Embedding> embedding, MemoryStore& store);

    // 动态更新配置与依赖（线程安全）
    void update(MemoryConfig cfg, std::shared_ptr<Embedding> embedding);
    void updateConfig(MemoryConfig cfg);
    void updateEmbedding(std::shared_ptr<Embedding> embedding);

    // 写入流程：摘要文本 → 向量化 → BLOB → INSERT（幂等：同会话同文本忽略）。
    // 失败只记日志（记忆是锦上添花，不允许影响对话主链路）。
    void remember(const ConversationKey& conv, const std::string& personId,
                  const std::string& summary, bool isPublic, std::int64_t now);

    // 检索流程：query 向量化 → SQL 预过滤 → SIMD 点积 + 时间衰减 → Top-K。
    // 失败返回空列表（= 本轮不注入记忆）。
    std::vector<RecalledMemory> recall(const std::string& query,
                                       const std::string& viewerPersonId,
                                       const std::string& convKey,
                                       std::int64_t now,
                                       std::size_t topK = 0);  // 0 = 用 cfg.topK

    std::size_t count();

    // 召回结果 → prompt 注入块（调用方以 ephemeral part 包裹，不落档案）
    static std::string renderForPrompt(const std::vector<RecalledMemory>& recalled);

private:
    // 归一化 + 维度校验；失败返回空向量
    std::vector<float> embedNormalized(const std::string& text) const;
    // 失败冷却（端点不可用时 60s 内不再发起请求，直接降级）
    bool embedOnCooldown() const;
    void noteEmbedResult(bool ok) const;

    MemoryConfig cfg_;
    std::shared_ptr<Embedding> embedding_;
    MemoryStore& store_;

    mutable std::mutex mtx_;  // 保护以下两员（网络调用在锁外）
    std::set<std::string> remembered_;  // 进程内幂等去重（convKey\x1fsummary）
    mutable std::chrono::steady_clock::time_point cooldownUntil_ =
        std::chrono::steady_clock::time_point::min();
    mutable bool lastEmbedFailed_ = false;  // 连续失败时降日志级别，防刷屏

    static constexpr std::size_t kRememberedCap = 8192;   // 去重集上限（防膨胀）
    static constexpr int kEmbedCooldownSeconds = 60;
    static constexpr std::size_t kParallelMinCandidates = 256;  // 多线程门槛
};

} // namespace mio
