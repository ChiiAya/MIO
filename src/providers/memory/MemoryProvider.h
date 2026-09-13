#pragma once
// ============================================================================
// MemoryProvider —— 经历记忆后端插件接口（共享契约，冻结）
//
// 定位：经历记忆（episodic memory）的存储、归纳与召回后端。
// 将来 Hindsight 只作为本接口的实现接入，【不接管】身份事实、关系权限、
// 审批或 prompt 预算 —— 那些仍由 MIO 控制。
//
// 插件约束（实现方必须遵守）：
//   * 插件 MUST NOT 读取 MIO 原始档案目录、关系图谱文件或审批字段；
//   * 插件只接收【已通过 MIO 权限过滤】的 EpisodeMemory 和 RecallQuery；
//   * 插件返回的内容必须经过 MIO 的可见性过滤、长度限制和注入格式化；
//   * 插件不可用时，MIO 必须降级为无长期召回运行，不得阻塞主对话；
//   * 插件返回的事实或关系判断只能转成 proposal，不能直接写入确认层；
//   * 接口约束不等于进程隔离：本 C++ 接口不提供沙箱，防不可信插件需另行
//     实现进程隔离，不得宣称当前接口已提供隔离。
// ============================================================================

#include "core/contracts/AccessContext.h"
#include "core/contracts/Errors.h"
#include "core/contracts/SummaryContracts.h"
#include "core/contracts/Visibility.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mio {

// 一条经历记忆（已由 MIO 完成权限与来源标注后交给插件）
struct EpisodeMemory {
    std::int64_t localId = 0;             // MIO 本地持久化 ID（来源 ID）
    std::string conversationKey;
    std::vector<std::string> participants;
    std::string text;                     // 已剔除 reasoning / 内部工具参数
    Visibility visibility = Visibility::Conversation;
    SummaryKind kind = SummaryKind::EpisodicMemory;
    std::int64_t fromMessageId = 0;
    std::int64_t toMessageId = 0;
    std::int64_t createdAt = 0;
    std::int64_t eventTime = 0;
    std::string source;                   // 例如 "silence_summary" / "model_tool"
    std::vector<float> embedding;         // 可为空；空则由后端自行向量化
    EmbeddingStatus embeddingStatus = EmbeddingStatus::Pending;
    std::string idempotencyKey;           // 幂等键（manual 记忆使用）
};

// 召回查询（权限条件已由 MIO 收敛到 AccessContext）
struct RecallQuery {
    std::string text;
    AccessContext access;
    std::size_t topK = 3;
    double minSimilarity = 0.30;
    std::int64_t now = 0;
    // context_compaction 默认不得进入长期召回；只有显式转换后才可召回
    bool allowContextCompaction = false;
};

struct MemoryHit {
    std::int64_t localId = 0;
    std::string text;
    std::string conversationKey;
    Visibility visibility = Visibility::Conversation;
    SummaryKind kind = SummaryKind::EpisodicMemory;
    double score = 0.0;
    std::int64_t createdAt = 0;
    std::string source;
};

enum class StoreStatus {
    Stored,    // 已确认落盘（必须有本地持久化依据）
    Queued,    // 已排队（同样必须有本地持久化依据，否则不得返回）
    Duplicate, // 幂等命中，返回既有记录 ID
    Failed,    // 失败（不得对模型返回"已记住"）
    Rejected,  // 被权限/校验拒绝
};

inline const char* toString(StoreStatus s) {
    switch (s) {
    case StoreStatus::Stored: return "stored";
    case StoreStatus::Queued: return "queued";
    case StoreStatus::Duplicate: return "duplicate";
    case StoreStatus::Failed: return "failed";
    case StoreStatus::Rejected: return "rejected";
    }
    return "failed";
}

struct StoreEpisodeResult {
    StoreStatus status = StoreStatus::Failed;
    std::int64_t localId = 0;
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    bool ok() const {
        return status == StoreStatus::Stored || status == StoreStatus::Queued ||
               status == StoreStatus::Duplicate;
    }
};

class MemoryProvider {
public:
    virtual ~MemoryProvider() = default;

    // 后端标识（"sqlite-local" / "hindsight" / "unavailable"）
    virtual std::string name() const = 0;

    // 可用性：false 时 MIO 降级为无长期召回运行，绝不阻塞主对话
    virtual bool available() const = 0;

    // 存储一条经历记忆。必须返回明确的成功/失败结果与本地 ID。
    virtual StoreEpisodeResult storeEpisode(const EpisodeMemory& episode) = 0;

    // 召回：入参已带权限上下文；返回内容仍需 MIO 二次过滤与长度限制。
    // 后端异常必须内部消化，返回空列表而不是抛出。
    virtual std::vector<MemoryHit> recall(const RecallQuery& query) = 0;
};

} // namespace mio
