#pragma once
// ============================================================================
// Achieve（会话档案）：只承担两件事 ——
//   1) 读取历史给 Fusion / 上下文构建器（冷启动：双重预算内恢复最近可见
//      原文；不调用摘要模型、不写长期记忆）；
//   2) 把消息保存到每会话独立的 jsonl（纯追加），并分配会话内稳定 ID。
// 不承担任何上下文构建 / 融合判断职责（严格模块边界）。
//
// 硬约束（docs/operations/memory-system-refactor.md）：
//   * 稳定会话内递增 messageId；引用统一用 (conversationKey, messageId)，
//     不得用昵称或时间戳代替唯一身份；
//   * 旧档案不改写：通过可重建旁路索引（<file>.idx.json）按行位置补 ID；
//     索引丢失/损坏时结果必须一致（ID 始终由文件内容确定性推导）；
//   * 损坏行必须报告位置（damageReport），不得把损坏后的消息静默当成不存在；
//   * 路径编码防会话键碰撞（百分号编码 + FNV-1a 短哈希），同时仍能读取旧命名
//     文件（读到就用它，新会话才用新命名）；
//   * 只有确认落盘成功的消息可进入总结范围：appendChecked 检查 ofstream
//     写入 + flush + close，失败时不得更新缓存与最大 ID；
//   * 读快照后释放锁，再调用模型或插件（本类内部不调用任何外部组件）。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "context/achieve/ColdStart.h"
#include "core/contracts/ArchiveRecord.h"
#include "core/conversation/Conversation.h"
#include "core/message/Message.h"

namespace mio {

// 追加结果：ok == false 时 error 非空且未落盘（缓存与最大 ID 不得前进）
struct AppendResult {
    bool ok = false;
    std::int64_t messageId = 0;
    std::string error;
};

// 启动枚举诊断：目录里有档案但无法无损反解会话键（不猜 key、不抛异常）
struct ArchiveDiscoveryDiagnostic {
    std::string fileName;
    std::string reason;
};

class Achieve : public IArchiveReader, public IColdStartSource {
public:
    // 未总结范围（供调度器冻结 SummaryJob / 计算 PendingRange 用）。
    // 注意：这是【档案层】的投影类型，嵌套在 Achieve 内，避免与调度器自己的
    // mio::PendingRange（context/lifecycle/ConversationLifecycle.h）重名冲突；
    // 消费方用 Achieve::PendingRange 或 auto 接收后按字段拷贝到调度器类型。
    struct PendingRange {
        std::int64_t fromMessageId = 0;  // 首条可总结消息（0 = 没有候选）
        // 范围内【最后一条可总结消息】的 ID（0 = 没有候选）。
        // 与 fromMessageId / count 同一口径：只算用户 + 可见助手消息；
        // 末尾只有工具 / 纯 reasoning / 框架提醒时不会指向它们。
        std::int64_t toMessageId = 0;
        std::int64_t count = 0;          // 候选条数（用户 + 可见助手）
    };

    explicit Achieve(std::filesystem::path dir);

    // ---- 写入 -------------------------------------------------------------
    // 确认落盘成功后才更新缓存与最大 ID；失败返回 ok=false
    AppendResult appendChecked(const ConversationKey& key, const Msg& msg);
    AppendResult appendChecked(const std::string& convKey, const Msg& msg);

    // 兼容包装：内部调用 appendChecked；失败时记录错误日志并抛
    // std::runtime_error（void 无法回传失败，必须让故障可观察）
    void append(const ConversationKey& key, const Msg& msg);
    void append(const ConversationKey& key, const std::vector<Msg>& msgs);

    // ---- 读取（返回值拷贝；持锁期间不调用任何外部组件）--------------------
    // 原始记录（含 reasoning）：仅供迁移/诊断。构建模型上下文必须走
    // coldStart() / IArchiveReader 投影（它们已剔除 reasoning 与工具回合）。
    std::vector<Msg> load(const ConversationKey& key);
    std::size_t count(const ConversationKey& key);

    // ---- 冷启动（无 LLM；见 ColdStart.h）----------------------------------
    ColdStartResult coldStart(const std::string& convKey,
                              const ColdStartOptions& opts) override;
    // ConversationKey 便捷重载（非接口方法）
    ColdStartResult coldStart(const ConversationKey& key,
                              const ColdStartOptions& opts);

    // ---- IArchiveReader（B/D 消费）----------------------------------------
    std::int64_t latestMessageId(const std::string& convKey) override;
    std::vector<ArchiveRecord> readRange(const std::string& convKey,
                                         std::int64_t fromMessageId,
                                         std::int64_t toMessageId,
                                         std::size_t limit) override;
    std::vector<ArchiveRecord> readRecent(const std::string& convKey,
                                          std::size_t limit) override;
    std::vector<ArchiveRecord> search(const std::string& convKey,
                                      const std::string& query,
                                      std::size_t limit) override;
    std::vector<ArchiveDamage> damageReport() const override;

    // ConversationKey 便捷重载
    std::int64_t latestMessageId(const ConversationKey& key);
    std::vector<ArchiveRecord> readRange(const ConversationKey& key,
                                         std::int64_t fromMessageId,
                                         std::int64_t toMessageId,
                                         std::size_t limit);
    std::vector<ArchiveRecord> readRecent(const ConversationKey& key,
                                          std::size_t limit);
    std::vector<ArchiveRecord> search(const ConversationKey& key,
                                      const std::string& query,
                                      std::size_t limit);

    // ---- F5：未总结范围 ---------------------------------------------------
    // count 只统计【用户 + 可见助手】消息：不计 reasoning、工具消息与框架提醒；
    // 只基于已确认落盘的消息（appendChecked 成功后才进缓存）。
    PendingRange pendingRange(const ConversationKey& key,
                              std::int64_t afterMessageId) const;
    PendingRange pendingRange(const std::string& convKey,
                              std::int64_t afterMessageId) const;

    // ---- 启动枚举（Runtime 启动驱动静默调度器 / 管理端观测）---------------
    // 已存在档案的会话键：只读，不创建任何文件、不分配 ID、不改写档案；
    // 输出按 key 排序且稳定；覆盖旧命名档案（靠旁路注册表登记的真实 key），
    // 目录扫描只采信"解码后再 encodeKey 完全一致"的新命名。
    // 绝不因为某个文件名无法反解而抛异常或让整个枚举失败。
    std::vector<std::string> conversationKeys() const;
    // 枚举诊断（可观察）：哪些档案没有被枚举、为什么（例如旧命名无法无损反解）
    std::vector<ArchiveDiscoveryDiagnostic> discoveryDiagnostics() const;

    // ---- 路径（公开给测试与 Runtime 诊断）--------------------------------
    // 新命名：每个非 [A-Za-z0-9._-] 字节 → %XX，再追加 "-" + FNV-1a 8 位十六进制
    static std::string encodeKey(const std::string& convKey);
    // 新命名的档案路径（不检查磁盘）
    static std::filesystem::path primaryPathFor(const std::filesystem::path& dir,
                                                const std::string& convKey);
    static std::filesystem::path primaryPathFor(const std::filesystem::path& dir,
                                                const ConversationKey& key);
    // 旧命名候选（平台前缀引入前/后的历史文件），用于只读兼容
    static std::vector<std::string> legacyFileNames(const std::string& convKey);
    // 实际使用路径：新命名存在 → 用它；否则任一旧命名存在 → 用它（继续追加、
    // 不搬家）；都不存在 → 新命名。同一会话在进程内解析结果稳定。
    static std::filesystem::path resolvePathFor(const std::filesystem::path& dir,
                                                const std::string& convKey);
    // 新命名反解（去掉尾部 "-<8位十六进制>" 后百分号解码）。
    // 只做格式解码，是否采信由调用方做 encodeKey 往返校验决定。
    static bool decodeKeyStem(const std::string& stem, std::string& out);
    // 旁路注册表文件名（放在档案目录内；不是档案，枚举时忽略）
    static const char* registryFileName();

private:
    struct Conversation {
        std::vector<Msg> msgs;            // 文件顺序（= 时间顺序），messageId 已写入
        std::vector<std::int64_t> lineIds; // 每个非空行的 ID（含损坏行占位）
        std::int64_t maxId = 0;
        std::filesystem::path path;
        bool loaded = false;
    };

    // 调用方必须已持 mtx_
    Conversation& ensureLoadedLocked(const std::string& convKey) const;
    void loadFileLocked(const std::string& convKey, Conversation& conv) const;
    void noteDamageLocked(const std::string& convKey, const std::string& path,
                          std::int64_t lineNumber, const std::string& reason) const;
    void writeIndexLocked(const std::string& convKey, const Conversation& conv) const;

    // ---- 旁路注册表（可重建；只用于启动枚举，读档案不依赖它）----
    void ensureRegistryLoadedLocked() const;  // 惰性读盘，容错
    void writeRegistryLocked() const;         // tmp + rename；失败只记日志
    void registerConversationLocked(const std::string& convKey,
                                    const std::filesystem::path& path) const;
    // 目录扫描：返回可采信的会话键，诊断写入 diagnostics（可空）
    std::vector<std::string> scanArchivesLocked(
        std::vector<ArchiveDiscoveryDiagnostic>* diagnostics) const;

    // 投影：只输出可见正文与来源元数据；reasoning 内容绝不出现
    static ArchiveRecord project(const Msg& m, const std::string& convKey);
    static std::string visibleText(const Msg& m);
    // 工具回合完整性：只保留"调用 + 结果"成对且闭合的回合，孤立 tool 消息
    // 一律丢弃（不得作为未完成调用重放）
    static std::vector<ArchiveRecord> dropOrphanToolRecords(
        std::vector<ArchiveRecord> records);

    std::filesystem::path dir_;
    mutable std::mutex mtx_;
    mutable std::map<std::string, Conversation> cache_;   // key: convKey 字符串
    mutable std::map<std::string, std::filesystem::path> resolvedPaths_;
    mutable std::vector<ArchiveDamage> damage_;
    // 旁路注册表：convKey → 档案文件名（不含目录，便于整体搬迁）。
    // 只是"启动枚举"的可重建索引：文件丢失/损坏时按 key 解析路径即可重建。
    mutable std::map<std::string, std::string> registry_;
    mutable bool registryLoaded_ = false;
};

} // namespace mio
