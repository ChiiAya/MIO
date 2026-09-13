#pragma once
// ============================================================================
// ArchiveRecord（共享，冻结）—— 会话档案的稳定投影
//
// 硬约束：
//   * 档案需要稳定的会话内递增消息 ID；引用统一使用 (conversationKey, messageId)，
//     不得用昵称或时间戳代替唯一身份。
//   * 新字段兼容旧记录，不批量改写唯一原档案；旧档案通过可重建的旁路索引
//     分配稳定 ID；损坏行必须报告位置，不得把损坏后的消息静默当成不存在。
//   * reasoning 永远不进入冷启动原始上下文、经历摘要输入或工具默认返回值，
//     不提供 include_reasoning 开关。
//   * 历史工具调用不作为未完成调用重放（避免孤立 tool 消息）。
//   * 只有确认落盘成功的消息可进入总结范围。
// ============================================================================

#include "core/message/Message.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mio {

struct ArchiveRecord {
    std::string conversationKey;          // ConversationKey::toString()
    std::int64_t messageId = 0;           // 会话内稳定递增（从 1 开始；0 = 无效）
    Role role = Role::User;
    std::string text;                     // 已剔除 reasoning 的可见正文
    std::int64_t createdAt = 0;           // epoch seconds
    std::string senderId;
    std::string senderName;
    std::string platform;
    std::string groupId;

    bool hasReasoning = false;            // 存在但被排除（可观察，不返回内容）
    bool isToolMessage = false;           // role == Tool
    std::vector<std::string> toolNames;   // 仅工具名，绝不携带完整内部参数
    bool truncated = false;               // 正文被 UTF-8 安全截断

    // 供记忆系统使用的最小投影
    bool isUserVisible() const { return role == Role::User; }
    bool isAssistantVisible() const { return role == Role::Assistant; }
};

// 会话内已提交的游标（对应内存/归档摘要游标）
struct ArchiveCursor {
    std::string conversationKey;
    std::int64_t lastMessageId = 0;  // 已提交消息的最大 ID（0 = 未提交任何消息）
};

// 损坏行报告：位置 + 原因（必须可观察，不得静默丢弃）
struct ArchiveDamage {
    std::string conversationKey;
    std::string filePath;
    std::int64_t lineNumber = 0;
    std::string reason;
};

// ---------------------------------------------------------------------------
// IArchiveReader：档案只读投影（F 实现于 context/achieve/Achieve，B/D 消费）
// 约定：
//   * 所有方法返回的 ArchiveRecord 均已剔除 reasoning 与完整工具参数；
//   * readRange 含端点；limit == 0 表示不限制（由调用方负责预算）；
//   * 读快照后立刻释放锁，再调用模型或插件。
// ---------------------------------------------------------------------------
class IArchiveReader {
public:
    virtual ~IArchiveReader() = default;

    // 会话内已成功落盘的最大消息 ID（0 = 尚无）
    virtual std::int64_t latestMessageId(const std::string& convKey) = 0;

    // 按消息 ID 区间读取（含端点，按 messageId 升序）
    virtual std::vector<ArchiveRecord> readRange(const std::string& convKey,
                                                 std::int64_t fromMessageId,
                                                 std::int64_t toMessageId,
                                                 std::size_t limit) = 0;

    // 最近 N 条（时间正序返回）
    virtual std::vector<ArchiveRecord> readRecent(const std::string& convKey,
                                                  std::size_t limit) = 0;

    // 有界全文检索（大小写不敏感子串；不返回 reasoning / 工具参数）
    virtual std::vector<ArchiveRecord> search(const std::string& convKey,
                                              const std::string& query,
                                              std::size_t limit) = 0;

    // 损坏行报告（可观察；空 = 无损坏）
    virtual std::vector<ArchiveDamage> damageReport() const = 0;
};

} // namespace mio
