#pragma once
// ============================================================================
// FusionRouter（会话融合路由）：fusion 实际上是一个路由
//
// 职责：
//   1、身份映射：将相同 internal id 的人的消息路由到同一 FusionUnit
//      （私聊同人跨平台 → 同一 unit；群聊按 平台+群ID 独立 unit）；
//   2、Fuse 判断：根据 RelationGraph（对话者熟悉程度 / 信任程度 / 话题相关度）
//      判断是否将不同的 IncomingMessage 来源路由到同一个 FusionUnit，
//      确认重定向时调用 FusionContext 的 beRedirected（redirect）函数；
//   3、创建 FusionUnit：某一次运行中第一次遇见某 IncomingMessage 来源时，
//      调用 Achieve 读取历史消息（冷读取：摘要前20条+原文5条）并创建新实例。
//   4、话题模糊语义匹配：可空依赖 Embedding，把"话题字符串相同"升级为
//      "话题语义相近"（余弦相似度），向量带缓存、失败自动降级。
//
// 结构：私有内存 —— 维护映射关系的数据结构（每个来源只能路由到一个 unit，
//       一个 unit 可以承担多个来源 —— FusionContext.inDegree 记录入度）。
// 对外接口：接入 llm-tool（setPublic，模型控制当前上下文公开/私密）；
//       接受 incomingMessage，重新组织映射关系并调用 redirect 融合上下文。
// 截断行为：设为私密时调用 redirect，但目标是原 fusionUnit（源自身 unit，冷启动重建）。
// ============================================================================

#include <cstddef>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/conversation/Conversation.h"
#include "core/event/Event.h"
#include "context/achieve/Achieve.h"
#include "context/conversationFusion/FusionContext.h"
#include "context/summarizor/SummaryManager.h"
#include "llm/Embedding.h"
#include "mind/graph/RelationshipGraph.h"

namespace mio {

struct FusionConfig {
    double intimacyThreshold = 0.6;  // 熟悉度阈值
    double trustThreshold = 0.4;     // 信任度阈值
    bool topicSemanticMatch = true;  // embedding 可用时启用话题语义匹配
    double topicSimilarityThreshold = 0.55;  // 话题余弦相似度阈值
};

class FusionRouter {
public:
    // embedding 为可空依赖：为空 / 失败 / 关闭时话题判断退化为精确匹配
    FusionRouter(RelationshipGraph& graph, Achieve& achieve,
                 SummaryManager& summary, FusionConfig cfg = {},
                 const Embedding* embedding = nullptr);

    // 路由一条消息：身份映射 → 首次遇见冷读取建 unit → 融合判断（可能 redirect）
    // → 返回目标 unit（调用方随后持 unit->mtx 处理消息）
    FusionUnit* route(const IncomingMessage& msg, std::time_t now);

    // 身份注入：统一身份码在图谱中有对应身份 → 返回其昵称，否则返回原始
    // senderId（wire 层标签渲染用；档案/身份归一仍用原始 senderId）
    std::string displayName(const IncomingMessage& msg) const;

    // 模型工具入口：设置当前来源对应 unit 的公开/私密状态。
    // 直接修改当前 unit 的 isPublic，不触发拆融合/冷启动。
    FusionUnit* setPublic(const ConversationKey& source, bool isPublic);

    std::size_t unitCount() const;

private:
    // 来源身份：私聊 → person:internalId；群聊 → group:平台:群ID
    std::string identityKey(const IncomingMessage& msg, std::time_t now);
    // 查找来源当前路由到的 unit（sourceToUnit / personToUnit）
    FusionUnit* findUnit(const IncomingMessage& msg) const;
    // 首次遇见：Achieve 冷读取 → 创建 unit 并登记映射
    FusionUnit* createFromColdRead(const IncomingMessage& msg, std::time_t now);
    // 融合判断（熟悉度 + 信任度 + 话题相关 + 双方公开）
    bool canFuse(FusionUnit& a, FusionUnit& b) const;
    double intimacyBetween(FusionUnit& a, FusionUnit& b) const;
    double trustBetween(FusionUnit& a, FusionUnit& b) const;
    // 话题模糊语义匹配：embedding 余弦相似度（向量缓存）；
    // 无 embedding / 冷却期 / 关闭时退化为 v1 精确匹配
    bool topicsRelated(const std::string& a, const std::string& b) const;
    // topic → 归一化向量（带缓存；失败返回空，空 = 回退精确匹配）。
    // 按值返回：缓存满清空时不会让调用方持有悬垂引用
    std::vector<float> topicVector(const std::string& topic) const;
    // 融合执行：短者 beRedirected → 追加到长者；inDegree/映射转移
    FusionUnit* fuse(FusionUnit& a, FusionUnit& b, std::time_t now);

    // ---- 私有内存：映射关系（每个来源→一个 unit；unit 可承载多个来源）----
    std::map<std::string, std::unique_ptr<FusionUnit>> units_;  // unitKey → unit
    std::map<std::string, std::string> sourceToUnit_;  // 来源 ConversationKey::toString → unitKey
    std::map<std::string, std::string> personToUnit_;  // 私聊 internalId → unitKey（同人同 unit）
    std::map<std::string, bool> unitIsPerson_;         // unitKey → 是否私聊身份（融合判定用）
    std::map<std::string, std::string> unitPersonId_;  // 私聊 unitKey → internalId
    std::map<std::string, std::set<std::string>> unitMembers_;  // 群 unitKey → 成员 internalId 集合
    mutable std::mutex mapMtx_;

    // 话题向量缓存（route 全程持 mapMtx_，天然串行；满则整体清空）
    mutable std::map<std::string, std::vector<float>> topicVecCache_;
    mutable std::time_t embedCooldownUntil_ = 0;  // 向量化失败冷却（保话题判断可用性）

    RelationshipGraph& graph_;
    Achieve& achieve_;
    SummaryManager& summary_;
    FusionConfig cfg_;
    const Embedding* embedding_ = nullptr;  // 非拥有；Runtime 组合根注入
};

} // namespace mio
