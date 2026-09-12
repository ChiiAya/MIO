#include "context/inputBuffer/inputBuffer.h"

#include <algorithm>
#include <cctype>

namespace mio {

namespace {

bool isAllWhitespace(const std::string& str) {
    return std::all_of(str.begin(), str.end(), [](unsigned char c) {
        return std::isspace(c);
    });
}

} // namespace

InputBuffer::InputBuffer(InputBufferConfig cfg) : cfg_(cfg) {
    pendingRaw_.reserve(8);
    pendingTurns_.reserve(8);
}

void InputBuffer::updateConfig(InputBufferConfig cfg) {
    std::lock_guard<std::mutex> lock(mtx_);
    cfg_ = cfg;
    cv_.notify_all();
}

PushResult InputBuffer::push(IncomingMessage msg, const std::string& displayName,
                             std::int64_t nowSec) {
    // 1. 过滤：全空白消息直接丢弃
    if (msg.text.empty() || isAllWhitespace(msg.text)) {
        return PushResult::Rejected;
    }

    // 2. 长文本安全过滤与截断
    if (msg.text.size() > cfg_.maxTextLength) {
        msg.text = msg.text.substr(0, cfg_.maxTextLength) + "...(消息过长已截断)";
    }

    // 3. 构建对应的上下文 Msg
    Msg turn{Role::User};
    turn.text = msg.text;
    turn.createdAt = nowSec;
    turn.senderId = msg.senderId;
    turn.senderName = displayName;
    turn.platform = msg.platform;
    turn.groupId = msg.groupId;

    std::lock_guard<std::mutex> lock(mtx_);
    const auto now = std::chrono::steady_clock::now();

    if (!hasLeader_) {
        // 首条到达的消息：此线程成为 Leader，负责随后的 waitForBatch 等待
        hasLeader_ = true;
        firstMsgTime_ = now;
        lastMsgTime_ = now;
        conversation_ = msg.conversation;

        pendingRaw_.push_back(std::move(msg));
        pendingTurns_.push_back(std::move(turn));
        return PushResult::AcceptedAsLeader;
    }

    // 后续跟随消息：追加进当前等待批次，并刷新最后到达时间
    lastMsgTime_ = now;

    // 若同一会话同一发言人连续打字，合并文本以节省 Prompt 与缓存
    if (!pendingTurns_.empty() && pendingTurns_.back().senderId == turn.senderId) {
        pendingTurns_.back().text += "\n" + turn.text;
    } else {
        pendingTurns_.push_back(std::move(turn));
    }

    pendingRaw_.push_back(std::move(msg));
    cv_.notify_all();
    return PushResult::AcceptedFollower;
}

BufferedBatch InputBuffer::waitForBatch() {
    std::unique_lock<std::mutex> lock(mtx_);

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const auto totalElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - firstMsgTime_).count();
        const auto idleElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMsgTime_).count();

        // 配置为 0 时不延迟，立即触发
        if (cfg_.debounceMs <= 0 || cfg_.maxDelayMs <= 0) {
            break;
        }

        // 达到最大延迟或条数达到上限时强制触发
        if (totalElapsed >= cfg_.maxDelayMs || pendingRaw_.size() >= cfg_.maxBatchSize) {
            break;
        }

        // 经过防抖静默期时触发
        if (idleElapsed >= cfg_.debounceMs) {
            break;
        }

        const auto waitDebounce = std::chrono::milliseconds(cfg_.debounceMs - idleElapsed);
        const auto waitMax = std::chrono::milliseconds(cfg_.maxDelayMs - totalElapsed);
        const auto waitDuration = std::min(waitDebounce, waitMax);

        cv_.wait_for(lock, waitDuration);
    }

    BufferedBatch batch;
    batch.rawMessages = std::move(pendingRaw_);
    batch.turns = std::move(pendingTurns_);
    batch.conversation = conversation_;

    pendingRaw_.clear();
    pendingTurns_.clear();
    hasLeader_ = false;

    return batch;
}

bool InputBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return pendingRaw_.empty();
}

void InputBuffer::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    pendingRaw_.clear();
    pendingTurns_.clear();
    hasLeader_ = false;
    cv_.notify_all();
}

// ============================================================================
// InputBufferManager 实现
// ============================================================================

InputBufferManager::InputBufferManager(InputBufferConfig cfg) : cfg_(cfg) {}

std::shared_ptr<InputBuffer> InputBufferManager::getOrCreate(const ConversationKey& conv) {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::string key = conv.toString();
    auto it = buffers_.find(key);
    if (it != buffers_.end()) {
        return it->second;
    }
    auto buf = std::make_shared<InputBuffer>(cfg_);
    buffers_[key] = buf;
    return buf;
}

void InputBufferManager::updateConfig(InputBufferConfig cfg) {
    std::lock_guard<std::mutex> lock(mtx_);
    cfg_ = cfg;
    for (auto& [_, buf] : buffers_) {
        buf->updateConfig(cfg);
    }
}

void InputBufferManager::cleanupIdle() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = buffers_.begin(); it != buffers_.end(); ) {
        if (it->second.use_count() == 1 && it->second->empty()) {
            it = buffers_.erase(it);
        } else {
            ++it;
        }
    }
}

const InputBufferConfig& InputBufferManager::config() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return cfg_;
}

} // namespace mio