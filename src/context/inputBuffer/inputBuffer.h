#pragma once
// ============================================================================
// InputBuffer：物理会话（Conversation）维度的前置消息防抖与批处理缓冲区
//
// 职责：
//   1. 物理通道防抖（Debounce）：收集物理通道 0.5~1.0s 内连续输入的消息；
//   2. 长文本过滤与安全截断（Filter）：对超长输入限制预算，过滤空消息；
//   3. 批次重组（Batching）：将短时连续输入打包为单次 LLM 请求上下文；
//   4. 真实身份保全：批次内每条原始消息保留独立发言人元数据。
// ============================================================================

#include "core/conversation/Conversation.h"
#include "core/event/Event.h"
#include "core/message/Message.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mio {

struct InputBufferConfig {
    int maxDelayMs = 800;             // 最大等待时间（从首条消息起算）
    int debounceMs = 400;             // 防抖静默窗口（有新消息则续期）
    std::size_t maxTextLength = 4096; // 单条文本过滤与截断上限
    std::size_t maxBatchSize = 16;    // 单批次最大收集消息数
};

enum class PushResult {
    AcceptedAsLeader, // 首条消息：当前线程成为 Batch Leader，负责等待与执行 LLM
    AcceptedFollower, // 跟随消息：成功追加到当前批次，当前线程可静默返回
    Rejected          // 被过滤丢弃（如纯空白消息）
};

struct BufferedBatch {
    std::vector<IncomingMessage> rawMessages; // 保留所有原始物理消息（各自落档案/关系图谱用）
    std::vector<Msg> turns;                  // 过滤并重组后的独立上下文 Msg 列表
    ConversationKey conversation;            // 物理会话 Key

    bool empty() const { return rawMessages.empty(); }
    std::size_t size() const { return rawMessages.size(); }
};

class InputBuffer {
public:
    explicit InputBuffer(InputBufferConfig cfg = {});
    ~InputBuffer() = default;

    // 禁用拷贝/移动，确保锁和条件变量安全
    InputBuffer(const InputBuffer&) = delete;
    InputBuffer& operator=(const InputBuffer&) = delete;

    // 尝试将一条消息投入缓冲区
    PushResult push(IncomingMessage msg, const std::string& displayName, std::int64_t nowSec);

    // Leader 线程阻塞等待当前批次窗口结束并取出完整批次
    BufferedBatch waitForBatch();

    // 检查当前是否有未刷新的消息
    bool empty() const;
    void clear();

    const InputBufferConfig& config() const { return cfg_; }
    void updateConfig(InputBufferConfig cfg);

private:
    InputBufferConfig cfg_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    bool hasLeader_ = false;
    std::vector<IncomingMessage> pendingRaw_;
    std::vector<Msg> pendingTurns_;
    ConversationKey conversation_;

    std::chrono::steady_clock::time_point firstMsgTime_;
    std::chrono::steady_clock::time_point lastMsgTime_;
};

// ============================================================================
// InputBufferManager：物理会话级别的输入防抖缓冲区管理器
//
// 职责：按 ConversationKey 维护各个独立物理通道的 InputBuffer 实例，
//       实现物理通道级别的防抖聚合，彻底解耦物理防抖与心智上下文融合。
// ============================================================================
class InputBufferManager {
public:
    explicit InputBufferManager(InputBufferConfig cfg = {});
    ~InputBufferManager() = default;

    InputBufferManager(const InputBufferManager&) = delete;
    InputBufferManager& operator=(const InputBufferManager&) = delete;

    std::shared_ptr<InputBuffer> getOrCreate(const ConversationKey& conv);
    void updateConfig(InputBufferConfig cfg);
    void cleanupIdle();

    const InputBufferConfig& config() const;

private:
    InputBufferConfig cfg_;
    mutable std::mutex mtx_;
    std::map<std::string, std::shared_ptr<InputBuffer>> buffers_;
};

} // namespace mio