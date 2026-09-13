// UnavailableMemoryProvider 实现（见 providers/memory/UnavailableMemoryProvider.h）
//
// 本实现是"诚实的失败"：不持久化、不召回、不抛异常，只用稳定错误码告诉调用方
// "后端不可用，请走无长期召回路径"。任何看似成功的返回值都会让模型以为自己
// 记住了东西，是本项目明令禁止的撒谎行为。

#include "providers/memory/UnavailableMemoryProvider.h"

#include "log/Log.h"

#include <utility>

namespace mio {

UnavailableMemoryProvider::UnavailableMemoryProvider(std::string reason)
    : reason_(reason.empty() ? std::string("backend unavailable") : std::move(reason)) {}

std::string UnavailableMemoryProvider::name() const { return "unavailable"; }

bool UnavailableMemoryProvider::available() const { return false; }

StoreEpisodeResult UnavailableMemoryProvider::storeEpisode(
    const EpisodeMemory& /*episode*/) {
    StoreEpisodeResult out;
    out.status = StoreStatus::Failed;
    out.localId = 0;  // 没有落盘就没有 ID，不得伪造
    out.code = ErrorCode::StorageUnavailable;
    out.message = "记忆后端不可用，未写入任何内容: " + reason_;
    log::warn("UnavailableMemoryProvider", out.message);
    return out;
}

std::vector<MemoryHit> UnavailableMemoryProvider::recall(
    const RecallQuery& /*query*/) {
    // 降级 = 无长期召回：直接返回空列表。本实现不做任何可能抛出的操作
    // （无 IO、无网络、无分配之外的逻辑），因此无需 catch 也不会抛出。
    return {};
}

} // namespace mio
