#pragma once
// ============================================================================
// 会话融合单元（FusionUnit / FusionContext）
//
// FusionContext 是【具有入度的结构】：inDegree 记录承载的对话 key
// （一个 unit 可以承担多个来源，一个来源只能路由到一个 unit）。
// Fusion 即把不同来源重定向到同一 unit（inDegree 合并 + 内容合并）。
//
// beRedirected：内容被重定向到其他 unit 时调用 —— 总结前 20 条 + 保留
// 最近 5 条原文，返回 msgs 交给目标 unit（与 Achieve 冷读取同一压缩形态）。
// reColdStart：上下文不够时由 contextBuilder 调用，压缩自身（重新冷启动）。
// ============================================================================

#include "core/conversation/Conversation.h"
#include "core/message/Message.h"

#include <mutex>
#include <string>
#include <vector>

namespace mio {

class SummaryManager;  // 见 context/summarizor/SummaryManager.h

struct FusionContext {
    std::vector<ConversationKey> inDegree;  // 承载的对话 key
    std::vector<Msg> context;
    std::string topic;
    bool isPublic = false;  // 冷启动默认私密；摘要判定后更新
    int turnsSinceCreation = 0;  // unit 创建以来的对话轮数（topic 预热用）
};

// 压缩结果：[摘要(前 summaryCount) + 近 rawKeep 原文] + 私密判定 + 话题。
// 话题由摘要器随摘要同步产出；非空时调用方回写 unit（见 beRedirected /
// reColdStart），使 topic 在冷读取/重定向/重新冷启动后保持最新，
// 不再只依赖模型主动调 update_topic。
struct CompressResult {
    std::vector<Msg> msgs;
    bool isPublic = false;
    std::string topic;
};

class FusionUnit {  // 对象：承载一个 FusionContext 的所有功能与内存
public:
    FusionUnit(const ConversationKey self, std::vector<Msg> context);

    // 内容被重定向到其他 unit 时调用：总结前 20 条 + 保留最近 5 条原文，返回 msgs
    std::vector<Msg> beRedirected(SummaryManager& summaryManager);
    // 无总结能力的重定向：仅保留最近 5 条原文（LLM 不可用/无需压缩时）
    std::vector<Msg> beRedirected();

    void append(Msg msg);  // 追加上下文

    // 剥离已完结回合中的临时片段（如图片 base64、临时注入的 ephemeral part），
    // 避免在多轮对话中向视觉模型重复上送历史图片造成冻结，并防止内存与上下文膨胀
    void stripEphemeralParts();

    // 重新冷启动（contextBuilder 触发）：压缩自身为 [摘要前20 + 近5原文]
    void reColdStart(SummaryManager& summaryManager);

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
    void addInDegree(const ConversationKey& key) {
        fusion_.inDegree.push_back(key);
    }
    void removeInDegree(const ConversationKey& key);
    void clearInDegree() { fusion_.inDegree.clear(); }
    void replaceContext(std::vector<Msg> context) {
        fusion_.context = std::move(context);
    }

    std::mutex mtx;  // 上下文级锁：同 unit 串行、跨 unit 并行

private:
    // 压缩实现：摘要前 summaryCount 条 + 近 rawKeep 条原文；
    // 返回 (msgs, 私密判定, 话题)
    static CompressResult compress(SummaryManager& summaryManager,
                                   const std::vector<Msg>& context,
                                   int summaryCount, int rawKeep);

    FusionContext fusion_;
};

} // namespace mio
