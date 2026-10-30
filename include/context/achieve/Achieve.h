#pragma once
// ============================================================================
// Achieve（全文事件档案库）：
//   1) 按单一时间线全量保存流入 AttentionGate 的 Event（大体积内联媒体自动外置）；
//   2) 提供按时间窗口为主、ID 为辅的查询接口，并支持一键格式化为 Tool 返回文本；
//   3) 冷读取（摘要前 summaryCount 条 + 保留近 rawKeep 条原文）仅在启动或超水位重冷启时调用。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "context/summarizor/SummaryManager.h"
#include "core/event/Event.h"

namespace mio {

struct AchieveConfig {
    bool enableColdSummary = false; // 冷启动是否触发总结+rawKeep截取（MindLoop按天总结，默认关闭）
    int summaryCount = 20;
    int rawKeep = 5;
    std::size_t inlineMediaMaxBytes = 4096;
};

struct ColdRead {
    std::vector<Msg> msgs;
    std::string topic;
};

class Achieve {
public:
    explicit Achieve(std::filesystem::path dir, AchieveConfig cfg = {});

    void setConfig(AchieveConfig cfg);
    AchieveConfig config() const;

    void append(Event ev);
    void append(const std::vector<Event>& events);

    // 按时间区间查询（target 为空查全部；非空时过滤 source / channelId / actorId）
    std::vector<Event> queryByTime(const std::string& target,
                                   std::int64_t startTime,
                                   std::int64_t endTime = 0,
                                   std::size_t limit = 50);

    std::vector<Event> queryRecentSeconds(const std::string& target,
                                          std::int64_t durationSeconds,
                                          std::size_t limit = 50);

    std::optional<Event> getById(std::uint64_t id);

    static std::string formatEventsForTool(const std::vector<Event>& events);

    ColdRead coldRead(const std::string& target = "", SummaryManager* summary = nullptr,
                      int summaryCountOverride = -1, int rawKeepOverride = -1);

    std::uint64_t maxId() const;

private:
    void loadFromDisk();
    void offloadInlineMedia(Event& ev) const;
    static std::int64_t normalizeToMs(std::int64_t ts);
    static bool matchesTarget(const Event& ev, const std::string& target);

    std::filesystem::path dir_;
    std::filesystem::path filePath_;
    AchieveConfig cfg_;
    std::vector<Event> events_;
    std::unordered_map<std::uint64_t, std::size_t> idIndex_; // Event::id -> events_ 下标
    std::uint64_t maxId_ = 0;
    mutable std::mutex mtx_;
};

} // namespace mio
