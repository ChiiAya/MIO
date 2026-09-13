// HindsightMemoryProvider 实现（见 providers/memory/HindsightMemoryProvider.h）
//
// HINDSIGHT_ADAPTER_TODO：本阶段只有占位。这里没有 socket、没有 HTTP 客户端、
// 没有 SDK、没有任何凭据读取 —— 任何"看起来联网"的代码都不允许出现在本文件。
// 接入真实 Hindsight 是后续独立工作项，必须另行评审配置/部署/传输与权限边界。

#include "providers/memory/HindsightMemoryProvider.h"

#include "log/Log.h"

namespace mio {

std::string HindsightMemoryProvider::name() const { return "hindsight"; }

bool HindsightMemoryProvider::available() const {
    return false;  // 未接入：永远不可用，Runtime 必须降级为无长期召回
}

const char* HindsightMemoryProvider::adapterTodoCode() { return kHindsightAdapterTodo; }

StoreEpisodeResult HindsightMemoryProvider::storeEpisode(
    const EpisodeMemory& /*episode*/) {
    StoreEpisodeResult out;
    out.status = StoreStatus::Failed;          // 也可 Rejected；这里表示"后端未实现"
    out.localId = 0;                           // 无落盘依据 → 不得返回 ID
    out.code = ErrorCode::HindsightAdapterTodo;
    out.message = std::string(kHindsightAdapterTodo) +
                  "：Hindsight 体验记忆后端尚未接入，未写入任何内容";
    {
        std::lock_guard<std::mutex> lock(mtx_);
        lastError_ = out.message;
    }
    log::warn("HindsightMemoryProvider", out.message);
    return out;
}

std::vector<MemoryHit> HindsightMemoryProvider::recall(
    const RecallQuery& /*query*/) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        lastError_ = std::string(kHindsightAdapterTodo) +
                     "：Hindsight 召回未接入，已降级为空结果（无长期召回）";
    }
    // 降级：返回空列表，不抛异常，不阻塞主对话链路。
    return {};
}

std::string HindsightMemoryProvider::lastError() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return lastError_;
}

} // namespace mio
