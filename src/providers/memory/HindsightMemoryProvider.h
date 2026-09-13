#pragma once
// ============================================================================
// HindsightMemoryProvider —— Hindsight 体验记忆后端【占位实现】（子任务 E）
//
// ⚠ HINDSIGHT_ADAPTER_TODO：Hindsight 的配置、部署、联网传输、真实 SDK 实现
//   全部未实现，本阶段【不实现、不联网、不引入任何新第三方依赖】。
//   本文件只是把"未接入"这一事实做成一个可观察、可降级、不撒谎的后端：
//     * name()      == "hindsight"（后端标识，冻结约定）
//     * available() == false（未接入，恒不可用；Runtime 据此走无长期召回）
//     * storeEpisode → Failed + ErrorCode::HindsightAdapterTodo
//                      （消费端可读 toString(code) == "HINDSIGHT_ADAPTER_TODO"）
//     * recall       → 空列表（降级），并把 HINDSIGHT_ADAPTER_TODO 记入 lastError
//                      （MemoryHit 没有错误码字段，故用可观察的 lastError 暴露）
//
// 未来真实接入时必须遵守的边界（现在就写死，避免实现时走样）：
//   * 只接收已通过 MIO 权限过滤的 EpisodeMemory / RecallQuery；
//   * MUST NOT 读取 MIO 原始档案目录、关系图谱文件或审批字段；
//   * 返回内容仍须由 MIO 复核可见性、截断长度并格式化注入，不得直接进 prompt；
//   * 返回的事实/关系判断只能转成 proposal，不得写 confirmed 层；
//   * 网络传输、鉴权、超时、重试与配置属于 HINDSIGHT_ADAPTER_TODO 的后续工作。
// 【接口约束不等于进程隔离】本 C++ 接口不提供沙箱：同进程加载的插件理论上仍能
// 打开宿主文件；若未来需要防不可信插件读取宿主文件，必须另行实现进程隔离，
// 不得宣称当前接口已提供隔离。
// ============================================================================

#include "core/contracts/Errors.h"
#include "providers/memory/MemoryProvider.h"

#include <mutex>
#include <string>
#include <vector>

namespace mio {

class HindsightMemoryProvider final : public MemoryProvider {
public:
    HindsightMemoryProvider() = default;

    std::string name() const override;  // "hindsight"
    bool available() const override;    // false（HINDSIGHT_ADAPTER_TODO）

    // Failed + ErrorCode::HindsightAdapterTodo（localId 恒为 0）。
    StoreEpisodeResult storeEpisode(const EpisodeMemory& episode) override;

    // 空列表（降级），并记录 HINDSIGHT_ADAPTER_TODO 供 Runtime/管理端观察。
    std::vector<MemoryHit> recall(const RecallQuery& query) override;

    // 最近一次调用的 TODO 说明（含错误码字符串），排障用；线程安全。
    std::string lastError() const;

    // 占位错误码字符串常量（"HINDSIGHT_ADAPTER_TODO"，与契约 Errors.h 同源）
    static const char* adapterTodoCode();

private:
    mutable std::mutex mtx_;
    std::string lastError_ = std::string(kHindsightAdapterTodo) +
                             "：Hindsight 未接入（配置/部署/联网/SDK 均未实现）";
};

} // namespace mio
