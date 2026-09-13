// 冷启动选择逻辑（见 context/achieve/ColdStart.h）
//
// 这里是纯函数：输入一份【时间正序】的档案投影快照，输出时间正序的 Msg 列表。
// 不碰文件、不碰模型、不碰锁 —— 便于单测证明"冷启动不调用摘要 LLM"。

#include "context/achieve/ColdStart.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "context/costEstimator/Tokens.h"
#include "core/contracts/Limits.h"

namespace mio {

namespace {

// 把 s 截到 estimateTokens(前缀) <= budget（UTF-8 安全）。
// 用"最大可行字节长度"的二分：pred(k) = estimateTokens(truncateUtf8(s,k)) <= budget
// 对 k 单调，标准二分即可；最终再走一次 truncateUtf8 保证不切碎多字节字符。
std::string truncateToTokens(const std::string& s, std::int64_t budget,
                             bool* truncated) {
    if (truncated != nullptr) *truncated = false;
    if (budget <= 0) {
        if (truncated != nullptr && !s.empty()) *truncated = true;
        return {};
    }
    if (estimateTokens(s) <= budget) return s;

    std::size_t lo = 0;
    std::size_t hi = s.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo + 1) / 2;
        if (estimateTokens(limits::truncateUtf8(s, mid)) <= budget)
            lo = mid;
        else
            hi = mid - 1;
    }
    std::string out = limits::truncateUtf8(s, lo);
    if (truncated != nullptr) *truncated = out.size() < s.size();
    return out;
}

// ArchiveRecord → Msg：只带可见正文与来源元数据。
// toolCalls 不还原（投影里只有工具名，没有 argumentsJson），因此不会把
// 历史工具调用变成可重放的未完成调用。
Msg toMsg(const ArchiveRecord& r) {
    Msg m;
    m.role = r.role;
    m.text = r.text;
    m.createdAt = r.createdAt;
    m.messageId = r.messageId;
    m.senderId = r.senderId;
    m.senderName = r.senderName;
    m.platform = r.platform;
    m.groupId = r.groupId;
    m.truncated = r.truncated;
    return m;
}

} // namespace

ColdStartResult selectColdStart(const std::vector<ArchiveRecord>& recordsAscending,
                                const ColdStartOptions& opts) {
    ColdStartResult out;
    // 0 = 不恢复历史原文（配置语义；不是"不限"）
    if (opts.rawTokenBudget <= 0 || opts.maxMessages <= 0) return out;

    const std::unordered_set<std::int64_t> excluded(opts.excludeMessageIds.begin(),
                                                    opts.excludeMessageIds.end());

    // 1) 可见性筛选（保持时间正序）。工具回合【整体排除】：
    //    带 toolCalls 的助手锚点与工具结果都不恢复，避免历史 tool 消息成为
    //    孤立未完成调用（需要工具证据时走有界查询工具）。
    std::vector<const ArchiveRecord*> candidates;
    candidates.reserve(recordsAscending.size());
    for (const auto& r : recordsAscending) {
        if (r.messageId > 0 && excluded.count(r.messageId) != 0) continue;
        if (r.isToolMessage) {
            ++out.droppedToolRounds;
            continue;
        }
        if (r.role == Role::Assistant && !r.toolNames.empty()) {
            ++out.droppedToolRounds;
            continue;
        }
        if (r.role != Role::User && r.role != Role::Assistant) {
            ++out.droppedNotVisible;
            continue;
        }
        if (r.text.empty() || isFrameworkReminder(r.text)) {
            ++out.droppedNotVisible;
            continue;
        }
        candidates.push_back(&r);
    }
    if (candidates.empty()) return out;

    // 2) 条数预算：只保留最近 maxMessages 条可见消息
    const std::size_t cap = static_cast<std::size_t>(opts.maxMessages);
    const std::size_t begin = candidates.size() > cap ? candidates.size() - cap : 0;
    out.droppedByBudget += static_cast<int>(begin);

    // 3) token 预算：从最新往旧累计，随后反转回时间正序
    std::vector<Msg> pickedRev;
    std::int64_t remaining = opts.rawTokenBudget;
    std::size_t idx = candidates.size();
    while (idx > begin) {
        --idx;
        Msg m = toMsg(*candidates[idx]);
        const std::int64_t cost = estimateTokens(m);
        if (cost <= remaining) {
            remaining -= cost;
            pickedRev.push_back(std::move(m));
            continue;
        }
        // 单条超预算：最新一条必须"截断保留"（既不能超预算，也不能整条
        // 消失）；更旧的一条放不下就停止，剩下的条数计入 droppedByBudget。
        if (pickedRev.empty() && remaining > 0) {
            bool cut = false;
            m.text = truncateToTokens(m.text, remaining, &cut);
            m.truncated = cut;
            if (cut) {
                out.anyTruncated = true;
                pickedRev.push_back(std::move(m));
            }
        }
        break;
    }
    out.droppedByBudget += static_cast<int>(
        (candidates.size() - begin) - pickedRev.size());
    if (pickedRev.empty()) return out;

    out.msgs.assign(pickedRev.rbegin(), pickedRev.rend());  // 时间正序返回
    out.fromMessageId = out.msgs.front().messageId;
    out.toMessageId = out.msgs.back().messageId;
    return out;
}

} // namespace mio
