#pragma once
// ============================================================================
// MemoryManager（记忆系统编排）：摘要写入 / 经历召回 / 向量化待办
//
// 写入流程（writeSummary）：
//   SummaryRecord ──> validateSummaryRecord ──> 可见性只可收窄
//                 ──> 单事务【摘要行 + 摘要游标 + 向量化待办 + 提案待投递】
//                 ──> 事务提交后再向量化（失败不回滚文本，只置 failed + 保留待办）
//
// 检索流程（recall）：
//   query ──> 向量化 + 归一化 ──> SQL 层可见性预过滤（conversation/person/public
//   + 当前会话 + 归属人，默认排除 context_compaction）
//         ──> 内存点积 + 时间衰减 ──> 【返回前二次可见性复核】──> Top-K
//
// 权限隔离：一条记忆可见 ⇔ 其 visibility 在 access.maxVisibility 之内，且
// conversation 记录属于可访问会话、person 记录属于请求者本人。不可见与不存在
// 统一返回空结果（不泄漏隐藏条数/摘要片段）。
//
// 降级承诺：embedding 端点不可用时 recall 返回空、写入保持 pending + 待办，
// 绝不向聊天主链路抛异常；连续失败进入 60s 冷却，不反复敲死端点。
// 向量化失败不得返回"已记住"：只有持久化失败才 ok=false。
// ============================================================================

#include "core/contracts/AccessContext.h"
#include "core/contracts/Limits.h"
#include "core/contracts/ProposalContracts.h"
#include "core/contracts/SummaryContracts.h"
#include "core/contracts/Visibility.h"
#include "core/conversation/Conversation.h"
#include "providers/embedding/Embedding.h"
#include "memory/store/MemoryStore.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mio {

struct MemoryConfig {
    std::size_t dim = 1024;                     // BGE-M3 维度（BLOB = dim*4 字节）
    std::size_t maxCandidates = 256;            // SQL 预过滤候选上限
    std::size_t topK = 3;                       // 注入 prompt 的记忆条数
    double minSimilarity = 0.30;                // 余弦门槛（低于丢弃，防垃圾注入）
    double decayTauSeconds = 30.0 * 24 * 3600;  // 时间衰减 τ（≈半月衰到 0.37）
    // ---- B 新增（缺省即最保守值；AppConfig 未接线时保持默认）----
    // 写入可见性上限：服务端只可收窄，模型/摘要不得放宽为 public。
    Visibility writeVisibilityCap = Visibility::Conversation;
    // 入库正文上限（与 validateSummaryRecord 的 64KB 一致，防止无界写入）
    std::size_t maxSummaryBytes = 64 * 1024;
    // renderForPrompt 注入块总字节上限与单条上限（prompt 预算有界）
    std::size_t promptMaxBytes = limits::kReadMaxBytes;
    std::size_t promptEntryMaxBytes = 1200;
    // 经历记忆后端（E 的 MemoryProvider 工厂）：sqlite-local（默认）/
    // unavailable / hindsight（占位）。未知名称由工厂降级为 unavailable，
    // 不属于"配置非法"（降级并记 WARN，不阻塞主对话）。
    std::string backend = "sqlite-local";
};

// 统一写入结果：ok=true 表示摘要文本已持久化（重复提交也返回原记录）；
// duplicate=true 时 memoryId 是【原记录 ID】。向量化失败不回滚文本：
// ok=true + embeddingStatus=Failed/Pending + message 说明待重试。
struct SummaryWriteResult {
    bool ok = false;
    bool duplicate = false;                 // 幂等命中（返回原记录 ID）
    std::int64_t memoryId = 0;
    EmbeddingStatus embeddingStatus = EmbeddingStatus::Pending;
    ErrorCode code = ErrorCode::Ok;
    std::string message;
};

struct RecalledMemory {
    std::int64_t id = 0;
    std::string convKey;
    std::string personId;
    Visibility visibility = Visibility::Conversation;
    SummaryKind kind = SummaryKind::EpisodicMemory;
    std::int64_t createdAt = 0;
    std::int64_t fromMessageId = 0;
    std::int64_t toMessageId = 0;
    std::string source;
    std::string summary;
    double similarity = 0;
    double score = 0;
};

class MemoryManager {
public:
    MemoryManager(MemoryConfig cfg, std::shared_ptr<Embedding> embedding, MemoryStore& store);
    void update(MemoryConfig cfg, std::shared_ptr<Embedding> embedding);
    void updateConfig(MemoryConfig cfg);
    void updateEmbedding(std::shared_ptr<Embedding> embedding);

    // 统一写入入口：校验 → 单事务（摘要+游标+向量化待办+提案待投递）→ 再向量化
    SummaryWriteResult writeSummary(const SummaryRecord& record);
    // 追加重载（不改变上面的冻结签名）：把摘要产出的事实/关系提案一并写入
    // proposal_outbox（同一事务），再由 deliverPendingProposals 投递给 C。
    SummaryWriteResult writeSummary(const SummaryRecord& record,
                                    const std::vector<Proposal>& proposals);

    std::optional<SummaryRecord> getSummary(std::int64_t memoryId);
    std::vector<SummaryRecord> listSummaries(const std::string& conversationKey,
                                             std::size_t limit,
                                             bool includeContextCompaction = false);
    std::int64_t committedCursor(const std::string& conversationKey);

    // 没有值得保留的内容时的可审计处理结果：推进游标 + 写审计标记，不产生记忆、不无限重试
    bool markRangeProcessed(const std::string& conversationKey, std::int64_t toMessageId,
                            const std::string& reason, std::int64_t now);

    std::size_t retryPendingEmbeddings(std::size_t limit, std::int64_t now);
    std::vector<RecalledMemory> recall(const std::string& query, const AccessContext& access,
                                       std::size_t topK = 0,
                                       bool includeContextCompaction = false);
    std::size_t count();
    static std::string renderForPrompt(const std::vector<RecalledMemory>& recalled);
    // 追加重载（不改变上面的冻结签名）：注入 prompt 前用 AccessContext 再复核一次
    // 可见性 —— 与 recall 的查询前过滤、返回前复核构成三道闸门。
    static std::string renderForPrompt(const std::vector<RecalledMemory>& recalled,
                                       const AccessContext& access,
                                       bool includeContextCompaction = false);

    // 兼容别名（安全）：走同一校验层（kind=manual、内容上限、可见性只可收窄），
    // 不得成为绕过新规则的旧入口
    SummaryWriteResult remember(const ConversationKey& conv, const std::string& personId,
                                const std::string& summary, bool isPublic, std::int64_t now);

    // 兼容别名（旧 recall 签名）：内部构造【最窄】AccessContext
    // （maxVisibility=Conversation、仅当前会话、requester=viewerPersonId），
    // 与新的 recall 走同一过滤与复核路径，绝不绕过权限层。
    std::vector<RecalledMemory> recall(const std::string& query,
                                       const std::string& viewerPersonId,
                                       const std::string& convKey, std::int64_t now,
                                       std::size_t topK = 0);

    // ---- 提案待投递（摘要产出 → C 的 ProposalStore）-----------------------
    // 至少一次投递：submit 成功才标记 delivered；失败记录 last_error 并保留待投递。
    std::size_t deliverPendingProposals(IProposalStore& proposals, std::size_t limit = 0);
    std::size_t pendingProposalCount();

private:
    // 归一化 + 维度校验；失败返回空向量
    std::vector<float> embedNormalized(const std::string& text) const;
    // 失败冷却（端点不可用时 60s 内不再发起请求，直接降级）
    bool embedOnCooldown() const;
    void noteEmbedResult(bool ok, const std::string& error) const;
    // 后端是否可用（未配置/冷却中 → 不消耗待办重试额度）
    bool embeddingReady() const;

    // 返回前二次可见性复核（与 SQL 过滤同一套语义）
    static bool isVisibleTo(const AccessContext& access, const RecalledMemory& r,
                            bool includeContextCompaction);
    // manual 记忆的内容幂等键（调用方未提供 idempotencyKey 时的兜底派生）
    static std::string deriveManualKey(const SummaryRecord& record);
    // 提案净化：服务端权威字段覆盖 + 审核占位清空（伪造字段丢弃）
    static Proposal sanitizeProposal(const Proposal& in, const SummaryRecord& record,
                                     std::int64_t now);

    MemoryConfig cfg_;
    std::shared_ptr<Embedding> embedding_;
    MemoryStore& store_;

    mutable std::mutex mtx_;  // 保护以下成员（网络调用在锁外）
    mutable std::chrono::steady_clock::time_point cooldownUntil_ =
        std::chrono::steady_clock::time_point::min();
    mutable std::string lastEmbedError_;  // 最近一次向量化失败原因（写入待办）

    static constexpr int kEmbedCooldownSeconds = 60;
    static constexpr std::size_t kParallelMinCandidates = 256;  // 多线程门槛
};

} // namespace mio
