#pragma once
// ============================================================================
// SummaryJob / SummaryRecord（共享，冻结）
//
// 硬约束：
//   * 静默摘要与上下文压缩摘要区分为不同 summary_kind；
//   * 强制字段：from_message_id / to_message_id / summary_kind / source /
//     visibility 不得为空；summary_kind 只允许 episodic_memory /
//     context_compaction / manual 三种，未知值拒绝写入；
//   * 摘要写入顺序：读取范围 → 生成结果 → 校验结果 → 原子写入摘要和游标
//     → 再触发向量化。向量化失败不得回滚摘要，但必须标记 embedding_status。
//   * 摘要必须按消息范围幂等（不能只按摘要文本去重）；
//   * context_compaction 默认不得进入长期召回，只有显式转换为
//     episodic_memory 才可被召回；
//   * 静默任务唯一键使用 会话 + 起止 ID + summary_kind；提交时比较旧游标，
//     防止重叠范围重复提交；相同范围重试返回原记录。
//   * 重试次数不含首次尝试；0 表示首次失败后不自动重试。
// ============================================================================

#include "core/contracts/ProposalContracts.h"
#include "core/contracts/Visibility.h"
#include "core/message/Message.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mio {

enum class SummaryKind {
    EpisodicMemory,     // 经历记忆（长期召回候选）
    ContextCompaction,  // 上下文压缩（不写经历、不推进静默游标、不提升可见性）
    Manual,             // 模型主动保存的经历（独立 idempotencyKey）
};

inline const char* toString(SummaryKind k) {
    switch (k) {
    case SummaryKind::EpisodicMemory: return "episodic_memory";
    case SummaryKind::ContextCompaction: return "context_compaction";
    case SummaryKind::Manual: return "manual";
    }
    return "";
}

// 严格解析：未知值返回 false（调用方必须拒绝写入，不得回退默认值）
inline bool parseSummaryKind(const std::string& s, SummaryKind& out) {
    if (s == "episodic_memory") { out = SummaryKind::EpisodicMemory; return true; }
    if (s == "context_compaction") { out = SummaryKind::ContextCompaction; return true; }
    if (s == "manual") { out = SummaryKind::Manual; return true; }
    return false;
}

// 只有经历记忆可进入长期召回（context_compaction 需显式转换后才可召回）
inline bool recallableByDefault(SummaryKind k) {
    return k == SummaryKind::EpisodicMemory;
}

enum class EmbeddingStatus {
    Pending,  // 已落库、等待向量化
    Ok,       // 向量化成功
    Failed,   // 向量化失败（文本保留，可重试）
};

inline const char* toString(EmbeddingStatus s) {
    switch (s) {
    case EmbeddingStatus::Pending: return "pending";
    case EmbeddingStatus::Ok: return "ok";
    case EmbeddingStatus::Failed: return "failed";
    }
    return "pending";
}

inline bool parseEmbeddingStatus(const std::string& s, EmbeddingStatus& out) {
    if (s == "pending") { out = EmbeddingStatus::Pending; return true; }
    if (s == "ok") { out = EmbeddingStatus::Ok; return true; }
    if (s == "failed") { out = EmbeddingStatus::Failed; return true; }
    return false;
}

// 摘要任务：静默调度器冻结范围后交给摘要执行者的完整输入
struct SummaryJob {
    std::string jobId;                    // 唯一任务 ID（服务端生成）
    std::string conversationKey;          // 物理会话（必须非空）
    std::vector<std::string> participants;// 参与者 internalId
    SummaryKind kind = SummaryKind::EpisodicMemory;
    std::int64_t fromMessageId = 0;       // 冻结范围起点（含）
    std::int64_t toMessageId = 0;         // 冻结范围终点（含）
    std::int64_t frozenAt = 0;            // epoch seconds（落盘时间，禁用 monotonic）
    std::uint64_t activityVersion = 0;    // 调度器活动版本（旧任务校验后退出）
    int attempt = 0;                      // 已重试次数（不含首次）
    Visibility visibility = Visibility::Conversation;
    std::string source = "silence_summary";

    // 幂等唯一键：会话 + 起止 ID + summary_kind
    std::string uniqueKey() const {
        return conversationKey + "\x1f" + std::to_string(fromMessageId) + "\x1f" +
               std::to_string(toMessageId) + "\x1f" + toString(kind);
    }
    bool valid() const {
        return !conversationKey.empty() && !jobId.empty() && fromMessageId > 0 &&
               toMessageId >= fromMessageId;
    }
};

// 落库后的经历/压缩摘要记录
struct SummaryRecord {
    std::int64_t memoryId = 0;            // 持久化 ID（写工具必须回传）
    std::string conversationKey;
    std::vector<std::string> participants;
    SummaryKind kind = SummaryKind::EpisodicMemory;
    std::string summary;
    Visibility visibility = Visibility::Conversation;
    std::int64_t createdAt = 0;
    std::int64_t eventTime = 0;
    std::int64_t fromMessageId = 0;
    std::int64_t toMessageId = 0;
    std::string source;
    EmbeddingStatus embeddingStatus = EmbeddingStatus::Pending;
    std::string idempotencyKey;           // manual 记忆使用；范围幂等用 uniqueKey
    // manual 记忆的来源消息引用（服务端校验后写入真实来源，模型只能提供允许读取的证据 ID）
    std::vector<std::string> evidenceMessageIds;  // 形如 "convKey#messageId"
    bool accepted = false;                // 是否本次真正写入（重复提交 = false）
};

// 摘要结果校验：强制字段不得为空、范围必须合法
struct SummaryValidation {
    bool ok = false;
    std::string reason;
};

SummaryValidation validateSummaryRecord(const SummaryRecord& rec);

// ---------------------------------------------------------------------------
// 摘要生成请求 / 结果（B 实现 SummaryManager::summarize，F/B/D 共同消费）
//
// 硬约束：
//   * material 必须已剔除 reasoning（摘要正文不得包含 reasoning、系统提示、
//     API key、内部工具实现细节或人工审核占位字段）；
//   * kind == ContextCompaction 的结果不写经历、不推进静默游标、不提升可见性；
//   * 摘要输出仅可建议【收窄】可见性，不能自动扩大为 public。
// ---------------------------------------------------------------------------
struct SummaryRequest {
    std::vector<Msg> material;                 // 指定消息范围（已过滤 reasoning）
    SummaryKind kind = SummaryKind::ContextCompaction;
    std::string conversationKey;
    std::vector<std::string> participants;
    std::int64_t fromMessageId = 0;
    std::int64_t toMessageId = 0;
    Visibility visibility = Visibility::Conversation;
    std::string source;                        // silence_summary / context_compaction / model_tool
    std::int64_t now = 0;
    // 允许模型建议收窄到的可见性（nullopt = 不建议）；不得用于放宽
    bool allowVisibilityNarrowing = true;
};

struct SummaryOutcome {
    std::string text;             // 摘要正文（标记行已剥离）
    // 摘要内容是否私密。默认 true（私密）：未解析 / 解析失败必须保持最窄范围，
    // 不得因为"没解析出来"而默认公开。
    bool privateVerdict = true;
    std::string topic;            // 摘要器同步产出的话题（解析失败/未输出为空）
    SummaryKind kind = SummaryKind::ContextCompaction;
    bool ok = false;              // 生成是否成功（失败时 text 可为空、error 非空）
    std::string error;            // 失败原因（成功时为空）
    // 摘要同步产出的事实提案与关系变化提案：只能进入 proposal 层（pending），
    // 绝不能直接写 confirmed 事实，也不能直接改变 relationship_type。
    std::vector<Proposal> factProposals;
    std::vector<Proposal> relationshipProposals;
};

} // namespace mio
