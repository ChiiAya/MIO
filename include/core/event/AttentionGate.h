// 注意力门：接受来自外部适配器/插件的 Event，裁决注意力层级并落盘 Achieve 后转入 Msg
// 同时持有 ID 递增器、动态规则表和 Digest 缓存池
// 【resolveAttention(attach id) -> achieve.append -> route to msgs -> drainMsgs】
#pragma once

#include "context/achieve/Achieve.h"
#include "core/event/Event.h"
#include <cstdint>
#include <mutex>
#include <vector>

namespace mio {

class AttentionGate {
public:
    static constexpr std::size_t kDefaultDigestBatchSize = 10;

    explicit AttentionGate(Achieve* achieve = nullptr);
    void bindAchieve(Achieve* achieve);

    // 外部传入的 Event 的 id 默认为 0；返回 true 表示产生了需要唤醒主循环的 Msg
    bool enterQueue(Event inEvt);

    // 取出当前已就绪的 Msg 队列（供 MindLoop 消费；取走时若 DigestBuffer_ 非空可顺便捎带冲刷）
    std::vector<Msg> drainMsgs(bool flushDigest = true);

    // 动态规则管理（供 LLM 工具或配置调用）
    void upsertRule(AttentionRule rule);
    bool removeRule(const std::string& pattern);
    std::vector<AttentionRule> listRules() const;

    // TODO: tool注册

private:
    bool resolveAttention(Event& inEvt); // 分配 ID 并根据动态规则更新 tier
    Msg DigestBuffer2Msg();
    std::uint64_t getIndex() { return ++index_; }

    mutable std::mutex mu_;
    Achieve* achieve_ = nullptr;
    std::vector<AttentionRule> rules_;
    std::vector<Event> DigestBuffer_;
    std::vector<Msg> readyMsgs_;
    std::uint64_t index_ = 0; // 不断递增，分配给 Event
};

} // namespace mio
