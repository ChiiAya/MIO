#pragma once
// ============================================================================
// UnavailableMemoryProvider —— 显式"无长期召回"降级桩（子任务 E，T12 用）
//
// 用途：插件未接入 / 后端初始化失败 / 配置了未知后端时，Runtime 用本实现保证：
//   * available() == false，调用方据此走"无长期召回"路径；
//   * storeEpisode 返回 Failed + STORAGE_UNAVAILABLE（或 Rejected），
//     **绝不**返回 Stored/Queued，也不声称"已记住"；
//   * recall 返回空列表且绝不抛异常（降级不阻塞主对话）。
//
// 插件约束：与其他后端一样只接收已过滤的 EpisodeMemory / RecallQuery；
// 本实现不读任何文件、不联网、不持有凭据。
// 【接口约束不等于进程隔离】本 C++ 接口不提供沙箱：同进程加载的插件理论上仍能
// 打开宿主文件；若未来需要防不可信插件读取宿主文件，必须另行实现进程隔离，
// 不得宣称当前接口已提供隔离。
// ============================================================================

#include "providers/memory/MemoryProvider.h"

#include <string>
#include <vector>

namespace mio {

class UnavailableMemoryProvider final : public MemoryProvider {
public:
    explicit UnavailableMemoryProvider(std::string reason = "backend unavailable");

    std::string name() const override;  // "unavailable"
    bool available() const override;    // false

    // Failed + STORAGE_UNAVAILABLE；localId 恒为 0，message 携带不可用原因。
    StoreEpisodeResult storeEpisode(const EpisodeMemory& episode) override;

    // 空列表，绝不抛异常。
    std::vector<MemoryHit> recall(const RecallQuery& query) override;

    // 降级原因（排障/管理端展示；不参与权限判断）
    const std::string& reason() const { return reason_; }

private:
    std::string reason_;
};

} // namespace mio
