#include "context/summarizor/SummaryManager.h"

#include <sstream>

#include "context/costEstimator/Tokens.h"

namespace mio {

namespace {

std::string renderHistoryText(const std::vector<Msg>& msgs, int maxChars) {
    std::ostringstream s;
    int budget = maxChars;
    for (const auto& m : msgs) {
        if (budget <= 0) break;
        std::string line;
        switch (m.role) {
        case Role::System: line = "[摘要] "; break;
        case Role::User: line = "[用户] "; break;
        case Role::Assistant: line = "[助手] "; break;
        case Role::Tool: line = "[工具(" + m.toolCallId + ")] "; break;
        }
        line += cutUtf8(m.text, 200);
        for (const auto& tc : m.toolCalls)
            line += " | 调用工具 " + tc.name + "(" + tc.argumentsJson + ")";
        line += "\n";
        budget -= static_cast<int>(line.size());
        if (budget >= 0 || s.tellp() == 0) s << line;
    }
    return s.str();
}

} // namespace

SummaryManager::SummaryManager(int maxSummaryChars, std::shared_ptr<Llm> llm)
    : maxSummaryChars_(maxSummaryChars), llm_(std::move(llm)) {}

SummaryOutcome SummaryManager::summarizeWithVerdict(const std::vector<Msg>& material) const {
    ChatRequest req;
    req.systemPrompt =
        "你是经历摘要器。把下面的对话经历压缩成一段中文【事实与时间线摘要】，"
        "要求：\n"
        "1) 按时间顺序概括发生过的重要事件、认识的人、人物关系变化、"
        "用户明确陈述过的事实与偏好、未完成的约定；\n"
        "2) 不保留对话原文、寒暄与闲聊细节，只提炼可长期记住的事实；\n"
        "3) 用陈述句概括，不要复述原文；\n"
        "4) 不超过 " + std::to_string(maxSummaryChars_ / 3) +
        " 字；\n"
        "5) 最后单独输出两行：\n"
        "话题：<十个字以内概括这段经历的主要话题>\n"
        "私密性判断：私密 或 公开"
        "（这段经历是否包含只适合对特定人群说的内容，如隐私、秘密、"
        "不宜公开的细节）。\n只输出摘要正文与这两行标记，不要其他前缀。";

    Msg m{Role::User};
    m.text = renderHistoryText(material, 20'000);
    req.messages.push_back(std::move(m));

    SummaryOutcome out;
    if (!llm_) return out;
    const ChatResponse resp = llm_->chat(req);
    const std::string full = cutUtf8(resp.text,
                                     static_cast<std::size_t>(maxSummaryChars_ * 2) + 32);

    // 从尾部逐行解析标记行（话题： / 私密性判断：），摘要正文 = 其余行。
    // 兼容：标记行缺失（对应项保持默认值）、两行顺序不定、全角/半角冒号、
    // 尾部多余空行。注意不能直接 find("私密")——标签"私密性判断"自身就含"私密"。
    std::vector<std::string> lines;
    {
        std::istringstream s(full);
        for (std::string line; std::getline(s, line);) lines.push_back(line);
    }
    while (!lines.empty()) {
        std::string line = lines.back();
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) {  // 尾部空行：剥离后继续看上一行
            lines.pop_back();
            continue;
        }
        const auto end = line.find_last_not_of(" \t\r");
        line = line.substr(begin, end - begin + 1);

        bool marker = false;
        if (line.rfind("私密性判断", 0) == 0) {
            const auto colon = line.find_first_of("：:");
            if (colon != std::string::npos) {
                out.privateVerdict =
                    line.substr(colon + 1).find("私密") != std::string::npos;
                marker = true;
            }
        } else if (line.rfind("话题", 0) == 0) {
            const auto colon = line.find_first_of("：:");
            if (colon != std::string::npos) {
                // 话题供 Router 语义匹配用，宜短；截断并去尾部空白
                std::string topic = line.substr(colon + 1);
                while (!topic.empty() &&
                       (topic.back() == ' ' || topic.back() == '\r' ||
                        topic.back() == '\t'))
                    topic.pop_back();
                out.topic = cutUtf8(topic, 50);
                marker = true;
            }
        }
        if (!marker) break;  // 正文行：标记区结束
        lines.pop_back();
    }
    {
        std::ostringstream joined;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) joined << '\n';
            joined << lines[i];
        }
        out.text = joined.str();
        // 去掉末尾空白
        while (!out.text.empty() &&
               (out.text.back() == '\n' || out.text.back() == '\r' ||
                out.text.back() == ' '))
            out.text.pop_back();
    }
    if (onSummary_) onSummary_(out);  // 摘要产出通知（记忆系统写入入口）
    return out;
}

} // namespace mio
