#pragma once
// ============================================================================
// Achieve（会话档案）：只承担两件事 ——
//   1) 读取历史记录给 Fusion（冷读取 = 摘要前 summaryCount 条 + 原文 rawKeep 条，可调）；
//   2) 把消息保存到 jsonl（每会话独立文件，纯追加）。
// 不承担任何上下文构建 / 融合判断职责（严格模块边界）。
// ============================================================================

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/conversation/Conversation.h"
#include "core/message/Message.h"
#include "context/summarizor/SummaryManager.h"

namespace mio {

struct ColdRead {
    std::vector<Msg> msgs;   // [摘要(前 summaryCount 条) + 原文(rawKeep 条)]；不足则纯原文
    bool isPublic = false;   // 摘要私密判定（无摘要时保持默认私密）
    std::string topic;       // 摘要器同步产出的话题（无摘要/未输出为空）
};

class Achieve {
public:
    explicit Achieve(std::filesystem::path dir);

    // 读取完整历史（jsonl）；不存在返回空列表
    std::vector<Msg> load(const ConversationKey& key);

    // 冷读取：摘要前 summaryCount 条 + 原文 rawKeep 条（可调）；
    // 历史不足 rawKeep 条时只返回原文、不调用 LLM
    ColdRead coldRead(const ConversationKey& key, SummaryManager& summary,
                      int summaryCount = 20, int rawKeep = 5);

    // 保存：jsonl 追加（线程安全）
    void append(const ConversationKey& key, const Msg& msg);
    void append(const ConversationKey& key, const std::vector<Msg>& msgs);

    std::size_t count(const ConversationKey& key);

private:
    std::vector<Msg>& ensureLoaded(const ConversationKey& key);
    static std::filesystem::path filePathFor(
        const std::filesystem::path& dir, const ConversationKey& key);

    std::filesystem::path dir_;
    std::map<std::string, std::vector<Msg>> cache_;  // key: ConversationKey::toString()
    mutable std::mutex mtx_;
};

} // namespace mio
