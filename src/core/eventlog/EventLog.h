#pragma once
// ============================================================================
// EventLog（程序事件审计日志）：append-only JSONL（events.jsonl）
//
// 只记录"程序级动作"（写日记 / 改昵称 / 改备注 / 摘要已应用），
// 【只写不读、不参与上下文构建】；seq 全局单调、随行落盘，
// 启动时从日志尾部恢复（下次写入接续）。
// 对话流（用户/助手/工具消息）的事实源是每会话的 jsonl 档案（Achieve）。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "core/event/Event.h"

namespace mio {

class EventLog {
public:
    explicit EventLog(std::filesystem::path file);

    // 分配 seq 并追加落盘（线程安全）；返回带 seq 的事件
    Event append(Event ev);

    std::uint64_t lastSeq() const { return lastSeq_; }

private:
    std::filesystem::path file_;
    std::uint64_t lastSeq_ = 0;
    std::ofstream out_;  // 保持打开，追加模式
    std::mutex mtx_;
};

} // namespace mio
