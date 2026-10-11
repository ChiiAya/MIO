#pragma once
// ============================================================================
// Scheduler（简易定时事项）
//   * 一个常驻线程 + 一张任务表；任务时刻按**本地时间**解释（与日志/日界一致）；
//   * 排程三种：once（某时刻执行一次）/ daily（每天某时刻）/ interval（每 N 分钟）；
//   * 动作由外部注入 —— 调度器不认识业务，只知道"到点了，把任务交给回调"；
//   * nextDailyAt / nextDue 是纯函数，可脱离线程单独验证排程边界
//     （月末/年末/闰日/DST/时钟回跳）。
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace mio {

enum class ScheduleKind { Once, Daily, Interval };

struct ScheduledTask {
    std::string name;              // 日志用标识（缺省回落到 action）
    std::string action;            // 动作名，由注入的回调解释（refresh_facts / prompt）
    std::string prompt;            // action == "prompt" 时的提示文本
    ScheduleKind kind = ScheduleKind::Daily;
    std::string at = "00:00";      // Once / Daily：本地 HH:MM
    int everyMinutes = 60;         // Interval
    bool enabled = true;
};

// 便捷构造：每天 at 执行一次
inline ScheduledTask dailyTask(std::string name, std::string action,
                               std::string at, std::string prompt = {}) {
    ScheduledTask task;
    task.name = std::move(name);
    task.action = std::move(action);
    task.prompt = std::move(prompt);
    task.kind = ScheduleKind::Daily;
    task.at = std::move(at);
    return task;
}

struct MaintenanceConfig {
    bool enabled = true;
    // 默认只有一条：每天 00:00 消费日记并重建 Facts。systemPrompt 在 wire 请求
    // 第一位，越少动越好（缓存前缀优先），所以通讯录/认知变更都折叠到这个日界；
    // 让模型整理记忆的 prompt 任务由用户按需在 config.json 里添加。
    std::vector<ScheduledTask> tasks = {dailyTask("daily-facts", "refresh_facts", "00:00")};
};

// ---- 纯函数（可单测）------------------------------------------------------
// "HH:MM" → 当天第几分钟；非法返回 -1
int parseClock(const std::string& hhmm);
// now 之后（今天或明天）的下一个 minuteOfDay 时刻，epoch 秒；非法返回 0
std::int64_t nextDailyAt(int minuteOfDay, std::int64_t now);
// 任务在 now 之后的下一次触发时刻；<=0 表示不再触发
std::int64_t nextDue(const ScheduledTask& task, std::int64_t now);

class Scheduler {
public:
    using Action = std::function<void(const ScheduledTask& task)>;

    Scheduler(std::vector<ScheduledTask> tasks, Action action);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    void start();
    void stop();  // 可重复调用；析构也会调，保证线程回收
    std::vector<ScheduledTask> tasks() const;

private:
    void loop();

    struct Entry {
        ScheduledTask task;
        std::int64_t nextAt = 0;  // epoch 秒；0 = 不再触发
    };

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<Entry> entries_;
    Action action_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
};

void to_json(nlohmann::json& j, const ScheduledTask& task);
void from_json(const nlohmann::json& j, ScheduledTask& task);
void to_json(nlohmann::json& j, const MaintenanceConfig& config);
void from_json(const nlohmann::json& j, MaintenanceConfig& config);

} // namespace mio
