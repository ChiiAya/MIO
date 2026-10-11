#include "scheduler/Scheduler.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <stdexcept>

#include "log/Log.h"

namespace mio {
namespace {

constexpr const char* kTag = "Scheduler";

// 时钟可能被 NTP 往回拨、也可能跨 DST，所以最长只睡这么久就醒来重算一次
constexpr std::int64_t kMaxSleepSec = 60;

std::int64_t nowEpoch() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string kindName(ScheduleKind kind) {
    switch (kind) {
    case ScheduleKind::Once: return "once";
    case ScheduleKind::Daily: return "daily";
    case ScheduleKind::Interval: return "interval";
    }
    return "?";
}

bool parseKind(const std::string& text, ScheduleKind& out) {
    if (text == "once") { out = ScheduleKind::Once; return true; }
    if (text == "daily") { out = ScheduleKind::Daily; return true; }
    if (text == "interval") { out = ScheduleKind::Interval; return true; }
    return false;
}

} // namespace

int parseClock(const std::string& hhmm) {
    const auto colon = hhmm.find(':');
    if (colon == std::string::npos) return -1;
    try {
        const int hour = std::stoi(hhmm.substr(0, colon));
        const int minute = std::stoi(hhmm.substr(colon + 1));
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
        return hour * 60 + minute;
    } catch (const std::exception&) {
        return -1;
    }
}

std::int64_t nextDailyAt(int minuteOfDay, std::int64_t now) {
    if (minuteOfDay < 0) return 0;
    const std::time_t t = static_cast<std::time_t>(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    tm.tm_hour = minuteOfDay / 60;
    tm.tm_min = minuteOfDay % 60;
    tm.tm_sec = 0;
    // 必须让 mktime 自己判定夏令时：沿用输入时刻的 isdst 会把目标时刻按旧时区
    // 解释（NY 3/8 01:30 的下一个 00:00 会算成 01:00）。
    tm.tm_isdst = -1;
    std::time_t candidate = std::mktime(&tm);
    if (candidate <= t) {  // 今天这个点已经过了 → 明天
        tm.tm_mday += 1;
        tm.tm_isdst = -1;
        candidate = std::mktime(&tm);
    }
    return static_cast<std::int64_t>(candidate);
}

std::int64_t nextDue(const ScheduledTask& task, std::int64_t now) {
    switch (task.kind) {
    case ScheduleKind::Interval:
        return now + static_cast<std::int64_t>(std::max(task.everyMinutes, 1)) * 60;
    case ScheduleKind::Once:
    case ScheduleKind::Daily:
        return nextDailyAt(parseClock(task.at), now);
    }
    return 0;
}

Scheduler::Scheduler(std::vector<ScheduledTask> tasks, Action action)
    : action_(std::move(action)) {
    const std::int64_t now = nowEpoch();
    for (auto& task : tasks) {
        if (!task.enabled) continue;
        if (task.name.empty()) task.name = task.action;
        Entry entry;
        entry.task = std::move(task);
        entry.nextAt = nextDue(entry.task, now);
        if (entry.nextAt <= 0) {
            log::warn(kTag, "任务排程非法，已忽略: name=" + entry.task.name +
                                " kind=" + kindName(entry.task.kind) +
                                " at=" + entry.task.at);
            continue;
        }
        entries_.push_back(std::move(entry));
    }
}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::start() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (running_) return;
    stopping_ = false;
    running_ = true;
    worker_ = std::thread([this] { loop(); });
    log::info(kTag, "已启动，任务数=" + std::to_string(entries_.size()));
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!running_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mtx_);
    running_ = false;
}

std::vector<ScheduledTask> Scheduler::tasks() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<ScheduledTask> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) out.push_back(entry.task);
    return out;
}

void Scheduler::loop() {
    std::unique_lock<std::mutex> lock(mtx_);
    while (!stopping_) {
        std::int64_t soonest = 0;
        for (const auto& entry : entries_) {
            if (entry.nextAt > 0 && (soonest == 0 || entry.nextAt < soonest))
                soonest = entry.nextAt;
        }
        if (soonest == 0) {  // 没有待触发任务：等 stop
            cv_.wait(lock);
            continue;
        }
        const std::int64_t now = nowEpoch();
        const std::int64_t wakeAt = std::min(soonest, now + kMaxSleepSec);
        cv_.wait_until(lock,
                       std::chrono::system_clock::from_time_t(
                           static_cast<std::time_t>(wakeAt)));
        if (stopping_) break;

        const std::int64_t after = nowEpoch();
        std::vector<ScheduledTask> due;
        for (auto& entry : entries_) {
            if (entry.nextAt <= 0 || entry.nextAt > after) continue;
            due.push_back(entry.task);
            // once 触发过就退休；daily 排到明天；interval 从"现在"再数
            entry.nextAt = entry.task.kind == ScheduleKind::Once
                               ? 0
                               : nextDue(entry.task, after);
        }
        lock.unlock();
        for (const auto& task : due) {
            log::info(kTag, "任务触发: name=" + task.name + " action=" + task.action);
            try {
                if (action_) action_(task);
            } catch (const std::exception& error) {
                log::error(kTag, "任务异常: name=" + task.name + " " + error.what());
            } catch (...) {
                log::error(kTag, "任务异常: name=" + task.name);
            }
        }
        lock.lock();
    }
}

void to_json(nlohmann::json& j, const ScheduledTask& task) {
    j = nlohmann::json{{"name", task.name},
                       {"action", task.action},
                       {"kind", kindName(task.kind)},
                       {"enabled", task.enabled}};
    if (!task.prompt.empty()) j["prompt"] = task.prompt;
    if (task.kind == ScheduleKind::Interval)
        j["everyMinutes"] = task.everyMinutes;
    else
        j["at"] = task.at;
}

void from_json(const nlohmann::json& j, ScheduledTask& task) {
    if (j.contains("name") && j["name"].is_string())
        task.name = j["name"].get<std::string>();
    if (j.contains("action") && j["action"].is_string())
        task.action = j["action"].get<std::string>();
    if (j.contains("prompt") && j["prompt"].is_string())
        task.prompt = j["prompt"].get<std::string>();
    if (j.contains("enabled") && j["enabled"].is_boolean())
        task.enabled = j["enabled"].get<bool>();
    if (j.contains("at") && j["at"].is_string())
        task.at = j["at"].get<std::string>();
    if (j.contains("everyMinutes") && j["everyMinutes"].is_number_integer())
        task.everyMinutes = j["everyMinutes"].get<int>();
    if (j.contains("kind") && j["kind"].is_string()) {
        ScheduleKind kind = task.kind;
        if (parseKind(j["kind"].get<std::string>(), kind)) task.kind = kind;
    } else if (j.contains("everyMinutes")) {
        task.kind = ScheduleKind::Interval;  // 只写间隔时的简写
    }
}

void to_json(nlohmann::json& j, const MaintenanceConfig& config) {
    j = nlohmann::json{{"enabled", config.enabled}, {"tasks", config.tasks}};
}

void from_json(const nlohmann::json& j, MaintenanceConfig& config) {
    if (j.contains("enabled") && j["enabled"].is_boolean())
        config.enabled = j["enabled"].get<bool>();
    if (j.contains("tasks") && j["tasks"].is_array()) {
        config.tasks.clear();
        for (const auto& item : j["tasks"]) {
            if (!item.is_object()) continue;
            ScheduledTask task;
            from_json(item, task);
            if (task.action.empty()) {
                log::warn(kTag, "忽略缺少 action 的任务配置");
                continue;
            }
            config.tasks.push_back(std::move(task));
        }
    }
}

} // namespace mio
