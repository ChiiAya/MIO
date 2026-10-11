#include "runtime/SummaryBridge.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "log/Log.h"

namespace mio {
namespace {

constexpr const char* kTag = "SummaryBridge";

// 攒批窗口：把"冷启动风暴"里同一波摘要合并成一次批量写入
constexpr int kGatherMs = 800;

} // namespace

SummaryBridge::SummaryBridge(MemoryBridgeConfig config, Writer writer)
    : config_(std::move(config)), writer_(std::move(writer)) {}

SummaryBridge::~SummaryBridge() {
    stop();
}

void SummaryBridge::start() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (running_) return;
    stopping_ = false;
    running_ = true;
    worker_ = std::thread([this] { loop(); });
    log::info(kTag, "已启动: tool=" + config_.tool + " textField=" + config_.textField);
}

void SummaryBridge::stop() {
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

std::size_t SummaryBridge::pending() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return pending_.size();
}

void SummaryBridge::push(Entry entry) {
    if (entry.text.empty()) return;
    const std::string key = entry.conversation.toString();
    std::lock_guard<std::mutex> lock(mtx_);
    if (const auto it = written_.find(key);
        it != written_.end() && it->second == entry.text) {
        return;  // 累计摘要与上次写出去的一模一样：没必要再写一次
    }
    if (const auto it = pending_.find(key);
        it != pending_.end() && it->second.entry.text == entry.text) {
        return;  // 队列里已有的还是同一份文本
    }
    const auto limit = static_cast<std::size_t>(std::max(config_.maxPending, 1));
    if (pending_.size() >= limit && pending_.find(key) == pending_.end()) {
        // 满了：丢最早入队的一条（按 seq，不是 map 的 key 序）
        auto oldest = pending_.begin();
        for (auto it = pending_.begin(); it != pending_.end(); ++it)
            if (it->second.seq < oldest->second.seq) oldest = it;
        log::warn(kTag, "待写入队列已满，丢弃最早入队的一条: " + oldest->first);
        pending_.erase(oldest);
    }
    pending_[key] = Pending{std::move(entry), nextSeq_++};
    cv_.notify_one();
}

void SummaryBridge::loop() {
    std::unique_lock<std::mutex> lock(mtx_);
    while (!stopping_) {
        if (pending_.empty()) {
            cv_.wait(lock);
            continue;
        }
        // 攒够一个固定窗口再取。注意不能用一次 wait_for —— 每次 push 的 notify
        // 都会把它提前唤醒，那就退化成"来一条写一条"（同一份累计摘要会重复写）。
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kGatherMs);
        while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
            cv_.wait_until(lock, deadline);
        }
        if (stopping_) break;
        std::map<std::string, Pending> batch;
        batch.swap(pending_);
        lock.unlock();
        for (const auto& [key, item] : batch) {
            const Entry& entry = item.entry;
            try {
                if (writer_ && !writer_(entry)) {
                    log::warn(kTag, "摘要写入记忆失败: " + key);
                } else {
                    log::info(kTag, "摘要已写入记忆: " + key +
                                        " bytes=" + std::to_string(entry.text.size()));
                    lock.lock();
                    written_[key] = entry.text;
                    writtenOrder_.emplace_back(key, entry.text);
                    // LRU 上界：只保留最近 256 条，避免长跑进程无限增长
                    while (writtenOrder_.size() > 256) {
                        const auto& front = writtenOrder_.front();
                        if (const auto it = written_.find(front.first);
                            it != written_.end() && it->second == front.second)
                            written_.erase(it);
                        writtenOrder_.pop_front();
                    }
                    lock.unlock();
                }
            } catch (const std::exception& error) {
                log::error(kTag, "摘要写入记忆异常: " + key + " " + error.what());
            } catch (...) {
                log::error(kTag, "摘要写入记忆异常: " + key);
            }
        }
        lock.lock();
    }
}


void to_json(nlohmann::json& j, const MemoryBridgeConfig& c) {
    j = nlohmann::json{{"enabled", c.enabled},
                       {"tool", c.tool},
                       {"textField", c.textField},
                       {"maxPending", c.maxPending}};
    if (!c.extraArgs.empty()) j["extraArgs"] = c.extraArgs;
}

void from_json(const nlohmann::json& j, MemoryBridgeConfig& c) {
    if (j.contains("enabled") && j["enabled"].is_boolean())
        c.enabled = j["enabled"].get<bool>();
    if (j.contains("tool") && j["tool"].is_string())
        c.tool = j["tool"].get<std::string>();
    if (j.contains("textField") && j["textField"].is_string())
        c.textField = j["textField"].get<std::string>();
    if (j.contains("maxPending") && j["maxPending"].is_number_integer())
        c.maxPending = j["maxPending"].get<int>();
    if (j.contains("extraArgs") && j["extraArgs"].is_object())
        c.extraArgs = j["extraArgs"];
}

} // namespace mio
