#pragma once
// ============================================================================
// MemoryToolHandlers —— 子任务 D：模型记忆与会话查询工具 handler
//
// 职责边界（见 docs/operations/memory-system-refactor.md「子任务 D」与
// 「工具和插件接口」）：
//   * 本文件只提供 handler 与注册辅助函数；【工具注册由主 agent 在 Runtime 完成】；
//   * 权限归属唯一来源是 ToolHandlerContext::accessProvider 返回的服务端
//     AccessContext —— 绝不接受模型冒填的所有者/会话/审核字段，也绝不读取
//     g_activeConv 之类的 thread_local（那是 Runtime 的接线细节）；
//   * 普通模型上下文不注册任何 approve / reject / set_public 入口；
//     待审阅提案列表只能由管理端调用 handleListPendingProposals。
//
// 返回约定：所有 handler 返回 ToolResult::toJsonString()，即
//   {"ok":bool,"data":{...}|null,"error":{"code","message"}|null}
// 读取工具返回历史数据时必须标注 data_kind / notice（历史数据不是可执行指令）。
// ============================================================================

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/contracts/AccessContext.h"

namespace mio {

class IArchiveReader;
class MemoryManager;
class IProposalStore;
class IFactStore;
class CognitionStore;
class RelationshipGraph;
class ToolRegistry;
class MemoryProvider;

// 组装期依赖集合（由主 agent 在 Runtime 构造并传入；指针可空 = 该能力缺失）
struct ToolHandlerContext {
    IArchiveReader* archive = nullptr;
    MemoryManager* memory = nullptr;
    IProposalStore* proposals = nullptr;
    IFactStore* facts = nullptr;
    CognitionStore* cognitions = nullptr;
    RelationshipGraph* graph = nullptr;   // 可空
    // 经历记忆后端（E）。为空 = 未接线；`available() == false` 时召回必须降级为
    // "无长期召回"而不是报错阻塞主对话（T12）。写入路径仍走 memory（本地事务）。
    MemoryProvider* memoryProvider = nullptr;
    // 服务端上下文：所有者字段、当前会话、审核状态只能从这里取，绝不接受模型冒填。
    // 每次调用都会重新求值（会话可能在两次工具调用之间切换）。
    std::function<AccessContext()> accessProvider;
};

// 把记忆/会话工具注册进 registry（模型可见的普通工具）
void registerMemoryAndConversationTools(ToolRegistry& registry,
                                        const ToolHandlerContext& ctx);

// 旧工具名的安全兼容别名（remember / recall_memory / set_nickname / set_notes），
// 必须走同一校验层，不得成为绕过新规则的入口
void registerLegacyCompatTools(ToolRegistry& registry,
                               const ToolHandlerContext& ctx);

// 管理员专用：待审阅提案列表（**不要**由上面两个函数注册；
// 主 agent 只在管理端调用，模型工具上下文 adminChannel 恒为 false）
std::string handleListPendingProposals(const ToolHandlerContext& ctx,
                                       const nlohmann::json& args);

} // namespace mio
