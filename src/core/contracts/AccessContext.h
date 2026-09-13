#pragma once
// ============================================================================
// AccessContext（共享，冻结）—— 所有记忆/档案读写的权限归属载体
//
// 硬约束：
//   * 后台工作必须【显式携带】会话、参与者、可见性和范围；
//     禁止读取 g_activeConv / g_activeMemoryPerson 等 thread_local 作为归属。
//   * 当前阶段历史查询默认仅限当前物理会话；跨会话查询暂不开放。
//   * 所有写工具的所有者字段、当前会话、审核状态都从本上下文取得，
//     不接受模型冒填。
// ============================================================================

#include "core/conversation/Conversation.h"
#include "core/contracts/Visibility.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mio {

struct AccessContext {
    // 当前物理会话（ConversationKey::toString()）；空 = 无会话归属（拒绝写入）
    std::string conversationKey;
    ConversationScope scope = ConversationScope::Private;

    // 请求者内部 ID（图谱 internalId；群聊可为空）。注意：不是平台昵称/QQ。
    std::string requesterPersonId;
    // 本次请求的原始平台身份（仅用于审计与身份解析，不参与权限判断）
    std::string platform;
    std::string platformUserId;
    std::string groupId;

    // 会话参与者（internalId 集合；群聊含所有已知成员）
    std::vector<std::string> participants;

    // 本次访问允许的最大可见性。任何返回记录必须 visibility <= maxVisibility。
    Visibility maxVisibility = Visibility::Conversation;

    // 跨会话访问：当前阶段恒为 false（预留字段，未来人工/管理员接口才可打开）
    bool allowCrossConversation = false;
    // 允许访问的会话白名单（空 且 !allowCrossConversation ⇒ 仅当前会话）
    std::vector<std::string> allowedConversationKeys;

    // 管理员通道：只有管理端 HTTP 接口等管理员通道才可置 true。
    // 模型工具调用注入的 AccessContext 必须恒为 false（不得由模型冒填）；
    // 待审阅提案列表等管理员专用数据以此为准。
    bool adminChannel = false;

    std::int64_t now = 0;  // epoch seconds

    bool hasConversation() const { return !conversationKey.empty(); }

    // 某会话是否在本次访问范围内（唯一权威判断，读工具必须调用）
    bool canAccessConversation(const std::string& convKey) const {
        if (convKey.empty()) return false;
        if (convKey == conversationKey) return true;
        if (!allowCrossConversation) return false;
        for (const auto& k : allowedConversationKeys) {
            if (k == convKey) return true;
        }
        return false;
    }

    // 记录可见性过滤：不可见与不存在统一返回 NOT_FOUND_OR_FORBIDDEN
    bool canSee(Visibility v) const {
        return static_cast<int>(v) <= static_cast<int>(maxVisibility);
    }
};

} // namespace mio
