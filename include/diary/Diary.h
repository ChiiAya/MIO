#pragma once
// ============================================================================
// 日记（封存式）：模型基础认知的"进步记录单元"。
//   * 认识新人/基础事实被刷新时，模型经工具 write_diary 写入；
//   * 只在摘要触发时消费（读取累计条目供 Facts 重建），消费即封存；
//   * 上限兜底：未消费条目超过 maxPending 时丢弃最旧。
// ============================================================================

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace mio {

struct DiaryEntry {
    std::uint64_t seq;
    std::int64_t createdAt;  // epoch 秒
    std::string text;
};

class Diary {
public:
    explicit Diary(std::filesystem::path file);

    // 模型工具 write_diary 的入口
    void add(const std::string& text, std::int64_t now);

    // 返回累计未消费条目并封存（移入已归档区，不再进入下一轮）
    std::vector<DiaryEntry> consume();

    std::size_t pendingCount() const;
    void setMaxPending(std::size_t n) { maxPending_ = n; }

    void save();

private:
    void load();
    void saveLocked();

    std::filesystem::path file_;
    std::vector<DiaryEntry> pending_;
    std::vector<DiaryEntry> archived_;
    std::uint64_t nextSeq_ = 1;
    std::size_t maxPending_ = 64;
    mutable std::mutex mtx_;  // 并发 ingest 共享（工具写入 / 摘要消费）
};

} // namespace mio
