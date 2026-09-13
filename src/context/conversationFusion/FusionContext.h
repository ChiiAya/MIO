#pragma once
// ============================================================================
// 会话融合单元（FusionUnit / FusionContext）
//
// FusionContext 是【具有入度的结构】：inDegree 记录承载的对话 key
// （一个 unit 可以承担多个来源，一个来源只能路由到一个 unit）。
// Fusion 即把不同来源重定向到同一 unit（inDegree 合并 + 内容合并）。
//
// 冷启动（F3）：unit 由 FusionRouter 用 Achieve 的无 LLM 冷启动创建
// （最近可见原文 + 双重预算），创建后 markColdStartDone()；ContextBuilder
// 对未标记的 unit 提供兜底冷启动。冷启动不再调用摘要模型、不写长期记忆，
// 因此 unit 初始 topic 为空、isPublic=false（默认私密）。
//
// 压缩（F4）：beRedirected / reColdStart 都按 kind = context_compaction 调
// SummaryManager::summarize，目标范围分块后逐块摘要并全部拼进结果，
// 不写经历、不推进静默游标、不提升可见性（只可收窄）。
// ============================================================================

#include "core/conversation/Conversation.h"
#include "core/message/Message.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mio {

class SummaryManager;  // 见 context/summarizor/SummaryManager.h
struct Usage;

struct UnitTokenStats {
    std::int64_t totalPromptTokens = 0;   // 累计输入 token（prompt_tokens）
    std::int64_t totalCachedTokens = 0;   // 累计命中缓存 token
    std::int64_t totalOutputTokens = 0;   // 累计输出 token
    std::int64_t lastPromptTokens = 0;    // 最近一轮输入 token
    std::int64_t lastCachedTokens = 0;    // 最近一轮命中缓存 token
    std::int64_t lastOutputTokens = 0;    // 最近一轮输出 token
    bool lastCacheHit = false;            // 最近一轮是否命中缓存
    int requestCount = 0;                 // 请求轮数
};

struct FusionContext {
    std::vector<ConversationKey> inDegree;  // 承载的对话 key
    std::vector<Msg> context;
    std::string topic;
    bool isPublic = false;  // 冷启动默认私密；摘要判定后只可收窄
    int turnsSinceCreation = 0;  // unit 创建以来的对话轮数（topic 预热用）
    std::vector<std::string> participants;  // 参与者 internalId（摘要请求用）
    bool coldStartDone = false;  // 是否已做过冷启动（ContextBuilder 兜底判据）
};

// 压缩选项：目标范围 = 除最后 rawKeep 条外的全部上下文
struct CompressOptions {
    int rawKeep = 5;          // 保留最近 N 条原文（不参与摘要）
    int chunkTokens = 5000;   // 每个摘要块的材料 token 预算（保证全覆盖）
    int maxChunks = 32;       // 分块上限；超出部分显式标记未覆盖（不静默丢弃）
    std::string source = "context_compaction";  // SummaryRequest.source
};

// 压缩结果：[分块摘要合并 + 近 rawKeep 原文] + 可见性建议 + 话题 + 覆盖情况
struct CompressResult {
    std::vector<Msg> msgs;
    bool isPublic = false;   // 建议值：调用方只能用它收窄，绝不能放宽
    std::string topic;
    bool fullCoverage = true;  // 目标范围是否全部被摘要覆盖
    std::vector<std::pair<std::int64_t, std::int64_t>> uncoveredRanges;
    int chunks = 0;        // 实际分块数（= summarize 调用次数）
    int failedChunks = 0;  // 摘要失败的块数（失败范围已写进正文标记）
};

class FusionUnit {  // 对象：承载一个 FusionContext 的所有功能与内存
public:
    FusionUnit(const ConversationKey self, std::vector<Msg> context);

    // 内容被重定向到其他 unit 时调用：压缩后返回 msgs（context_compaction）
    std::vector<Msg> beRedirected(SummaryManager& summaryManager,
                                  CompressOptions opts = {});
    // 无总结能力的重定向：仅保留最近 5 条原文（LLM 不可用/无需压缩时）
    std::vector<Msg> beRedirected();

    void append(Msg msg);  // 追加上下文

    // 重新冷启动（contextBuilder 触发）：按 context_compaction 压缩自身
    CompressResult reColdStart(SummaryManager& summaryManager,
                               CompressOptions opts = {});

    // 访问器 / 修改器
    const std::vector<Msg>& context() const { return fusion_.context; }
    const std::vector<ConversationKey>& inDegree() const {
        return fusion_.inDegree;
    }
    const std::string& topic() const { return fusion_.topic; }
    bool isPublic() const { return fusion_.isPublic; }
    // unit 创建以来的对话轮数（FusionRouter 在 route 时递增；
    // 融合重定向进来的消息不计入）
    int turnsSinceCreation() const { return fusion_.turnsSinceCreation; }
    void bumpTurn() { ++fusion_.turnsSinceCreation; }
    void setTopic(std::string topic);
    void setIsPublic(bool isPublic);

    // 冷启动标记：由做冷启动的一方置位（FusionRouter / ContextBuilder）
    bool coldStartDone() const { return fusion_.coldStartDone; }
    void markColdStartDone() { fusion_.coldStartDone = true; }

    // 参与者（摘要请求归属用；FusionRouter 在建立 unit / 融合时维护）
    const std::vector<std::string>& participants() const {
        return fusion_.participants;
    }
    void setParticipants(std::vector<std::string> participants) {
        fusion_.participants = std::move(participants);
    }
    void mergeParticipants(const std::vector<std::string>& more);

    void addInDegree(const ConversationKey& key) {
        fusion_.inDegree.push_back(key);
    }
    void removeInDegree(const ConversationKey& key);
    void clearInDegree() { fusion_.inDegree.clear(); }
    void replaceContext(std::vector<Msg> context) {
        fusion_.context = std::move(context);
    }

    UnitTokenStats stats() const {
        std::lock_guard<std::mutex> lock(statsMtx_);
        return stats_;
    }
    void recordUsage(const Usage& usage);

    std::mutex mtx;  // 上下文级锁：同 unit 串行、跨 unit 并行

private:
    // 压缩实现：目标范围 [0, n-rawKeep) 分块摘要，近 rawKeep 条原文保留
    CompressResult compress(SummaryManager& summaryManager,
                            const std::vector<Msg>& context,
                            const CompressOptions& opts);

    FusionContext fusion_;
    UnitTokenStats stats_;
    mutable std::mutex statsMtx_;
};

} // namespace mio
