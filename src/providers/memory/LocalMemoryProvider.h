#pragma once
// ============================================================================
// LocalMemoryProvider —— 经历记忆的本地（SQLite）后端适配器（子任务 E，默认后端）
//
// 定位：MemoryProvider 的默认实现。它只做三件事 —— 契约映射、返回前的权限/长度
// 复核、失败降级；**不自己访问数据库**：所有读写都经 MemoryManager
// （摘要行 + 游标 + 向量化待办 + 提案待投递在同一 SQLite 事务，召回在 SQL 层
// 预过滤），因此本适配器天然没有读取档案目录、关系图谱或审批字段的能力。
//
// 插件约束（与 MemoryProvider.h 的冻结约定一致，实现必须遵守）：
//   * 本适配器 MUST NOT 读取 MIO 原始档案目录、关系图谱文件或审批字段；
//     实现文件不得出现文件系统/档案行解析/图谱读取入口（见 E4 审查用例
//     E_plugin_sources_touch_no_archive_or_graph_files）。
//   * 只接收已由 MIO 权限层收敛过的 EpisodeMemory / RecallQuery，不自行拼装
//     权限上下文，也不读取 thread_local 归属。
//   * 返回内容必须经 MIO 复核可见性、截断长度后再注入 prompt（第三道闸门）。
//   * 后端异常一律内部消化：写入失败返回 Failed/Rejected，召回失败返回空列表，
//     绝不向主对话链路抛异常，也绝不把失败说成"已记住"。
//   * 【接口约束不等于进程隔离】本 C++ 接口不提供沙箱：同进程加载的插件理论上
//     仍能打开宿主文件。若未来需要防不可信插件读取宿主文件，必须另行实现进程
//     隔离，不得宣称当前接口已提供隔离。
//
// 依赖白名单（E4 代码审查锚点；测试会断言这些实现文件只 include 下列头）：
//   memory/manager/MemoryManager.h、providers/memory/MemoryProvider.h、
//   core/contracts/Limits.h、log/Log.h，以及 C++ 标准库（不含文件系统头）。
//
// 调用方契约（本适配器不代填、不推测）：
//   * conversationKey / text / source 必须非空，fromMessageId、toMessageId 必须
//     > 0 且 from <= to（契约 validateSummaryRecord 的强制字段）；范围非法时
//     返回 Rejected，**不推测、不伪造消息 ID**；
//   * 写入正文超过 limits::kWriteMaxBytes（4000 字节）返回 Rejected +
//     LIMIT_EXCEEDED（文档：超限写入返回 LIMIT_EXCEEDED，不做静默截断）；
//   * localId > 0 表示服务端已知的记录：按幂等返回 Duplicate（会复核记录是否存在
//     且属同一会话，避免接受伪造/跨会话 ID）。
//
// 线程安全：本类无可变成员（config 为构造时快照），MemoryManager 内部自持锁。
// ============================================================================

#include "core/contracts/Limits.h"
#include "memory/manager/MemoryManager.h"
#include "providers/memory/MemoryProvider.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mio {

class LocalMemoryProvider final : public MemoryProvider {
public:
    // memory 必须比本对象活得久（Runtime 组合根保证生命周期）；
    // config 用于召回 topK/相似度门槛、单条与总量长度预算，并作为写入可见性上限
    // （只可收窄，不得放宽）。
    LocalMemoryProvider(MemoryManager& memory, MemoryConfig config);

    std::string name() const override;  // "sqlite-local"

    // 本地后端不依赖网络/凭据：依赖由组合根注入即视为就绪，故恒为 true。
    // 单次存储/召回失败以 StoreEpisodeResult / 空召回表达，不据此把整个后端
    // 下线（读路径可能仍可用），Runtime 的降级依据是 available()==false。
    bool available() const override;

    // EpisodeMemory → SummaryRecord → MemoryManager::writeSummary。
    // 返回 Stored / Duplicate / Failed / Rejected 四态之一 + localId + 稳定错误码。
    StoreEpisodeResult storeEpisode(const EpisodeMemory& episode) override;

    // 转发 MemoryManager::recall（查询前 SQL 过滤 + 返回前复核），再做第三道
    // 闸门：canSee + canAccessConversation + compaction 开关 + 相似度门槛 +
    // 单条/总量截断。异常一律吞掉，返回空列表（降级为无长期召回）。
    std::vector<MemoryHit> recall(const RecallQuery& query) override;

    // E4 代码审查锚点：本适配器允许 include 的头文件白名单（测试断言用）。
    static const std::vector<std::string>& allowedIncludes();

private:
    // 纯映射（无 IO，便于审查）：只复制契约字段，不新增来源、不推测 ID、
    // 不放宽可见性（narrower(episode.visibility, cfg_.writeVisibilityCap)）。
    SummaryRecord toRecord(const EpisodeMemory& episode, std::int64_t now) const;

    MemoryManager& memory_;
    MemoryConfig cfg_;
};

} // namespace mio
