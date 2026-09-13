// SummaryManager 实现（见 context/summarizor/SummaryManager.h）
//
// 三条硬约束贯穿本文件：
//   1) reasoning 绝不进入摘要材料（材料用副本构造并清空 reasoningContent）；
//   2) 摘要正文写库前必须过 SummarySanitizer（reasoning 回显/凭据/提示词片段/
//      人工审核占位字段一律不得写入）；
//   3) 提案字段只能由模型提供 predicate/object/confidence，其余（subjectId、
//      conversationKey、source、evidenceMessageIds、可见性、审核占位）全部由
//      服务端按 SummaryRequest 填充；模型伪造的审核人/审核时间被丢弃。
// 失败一律转 ok=false + error，绝不让异常穿透到调度线程。

#include "context/summarizor/SummaryManager.h"

#include <cstdlib>
#include <optional>
#include <sstream>
#include <utility>

#include "context/summarizor/SummarySanitizer.h"

namespace mio {

namespace {

constexpr std::size_t kMaterialMaxBytes = 20'000;   // 材料总预算（避免 prompt 无界增长）
constexpr std::size_t kMaterialLineBytes = 200;     // 单条消息正文进入材料的字节上限
constexpr std::size_t kProposalFieldBytes = 160;    // 提案 predicate/object 上限
constexpr std::size_t kTopicBytes = 50;             // 话题字节上限（Router 语义匹配用）

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return out;
}

// 标记行解析：冒号后取值。
// 历史缺陷：line.find_first_of("：:") 会把全角冒号"："（3 字节）的首字节当成
// 匹配位，substr(colon+1) 从续字节开始 → topic 以 0xBC 开头的非法 UTF-8。
// 现改为先找 "："（整体），再回退 ':'，并按字节长度跳过冒号。
std::optional<std::string> markerValue(const std::string& line, const std::string& label) {
    const std::size_t full = line.find("：");
    const std::size_t half = line.find(':');
    std::size_t pos = std::string::npos;
    std::size_t colonBytes = 0;
    if (full != std::string::npos && (half == std::string::npos || full < half)) {
        pos = full;
        colonBytes = 3;  // "：" 的 UTF-8 长度
    } else if (half != std::string::npos) {
        pos = half;
        colonBytes = 1;
    }
    if (pos == std::string::npos) return std::nullopt;
    // 冒号前必须恰好是标签（容忍标签与冒号之间的空白），
    // 否则 "话题涉及…：…" 这类正文行会被误判成标记行。
    if (trim(line.substr(0, pos)) != label) return std::nullopt;
    return line.substr(pos + colonBytes);
}

// 一条模型输出的原始提案（只含允许模型提供的字段）
struct RawProposal {
    std::string predicate;
    std::string object;
    double confidence = 0.0;
    bool droppedReviewerFields = false;  // 模型夹带了 actor=/reviewedAt= 等，已丢弃
};

// "predicate | object | confidence"（分隔符兼容全角 ｜）
bool parseRawProposal(const std::string& value, RawProposal* out) {
    std::string v = value;
    std::size_t p = 0;
    while ((p = v.find("｜", p)) != std::string::npos) {  // 全角竖线 → 半角
        v.replace(p, 3, "|");
        ++p;
    }
    const std::string lower = toLowerAscii(v);
    // 伪造审核字段：丢弃该字段而不是整条提案（字段不是模型可以提供的内容）
    if (lower.find("actor=") != std::string::npos ||
        lower.find("reviewer") != std::string::npos ||
        lower.find("approver") != std::string::npos ||
        lower.find("approvedby") != std::string::npos ||
        lower.find("approved_by") != std::string::npos ||
        lower.find("reviewedat") != std::string::npos ||
        lower.find("reviewed_at") != std::string::npos ||
        lower.find("review=") != std::string::npos ||
        lower.find("review_status") != std::string::npos)
        out->droppedReviewerFields = true;

    std::vector<std::string> parts;
    std::string cur;
    for (char c : v) {
        if (c == '|') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    if (parts.size() < 2) return false;
    out->predicate = trim(parts[0]);
    out->object = trim(parts[1]);
    if (parts.size() >= 3) {
        try {
            out->confidence = std::stod(trim(parts[2]));
        } catch (...) {
            out->confidence = 0.0;
        }
    }
    if (!(out->confidence >= 0.0)) out->confidence = 0.0;   // NaN/负数 → 0
    if (out->confidence > 1.0) out->confidence = 1.0;
    return !out->predicate.empty() && !out->object.empty();
}

// prompt 里给出的占位示例（"<predicate> | <object>"）被模型原样回抄时必须丢弃
bool looksLikePlaceholder(const std::string& s) {
    if (s.find('<') != std::string::npos || s.find('>') != std::string::npos) return true;
    const std::string lower = toLowerAscii(s);
    return lower == "predicate" || lower == "object" || lower == "confidence" ||
           lower == "predicate>" || lower == "object>";
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::ostringstream s;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) s << '\n';
        s << lines[i];
    }
    return s.str();
}

} // namespace

SummaryManager::SummaryManager(int maxSummaryChars, std::shared_ptr<Llm> llm)
    : maxSummaryChars_(maxSummaryChars), llm_(std::move(llm)) {}

const char* SummaryManager::kindLabel(SummaryKind kind) {
    return toString(kind);
}

SummaryOutcome SummaryManager::summarize(const SummaryRequest& request) {
    SummaryOutcome out = generateFor(request);
    out.kind = request.kind;
    if (out.text.empty() && out.error.empty()) out.error = "摘要正文为空";
    out.ok = out.ok && !out.text.empty();
    if (!out.ok) {
        // 失败不得产出提案：生成失败的提案没有可信证据
        out.factProposals.clear();
        out.relationshipProposals.clear();
    }
    // 通知只在公开入口发一次（generate/generateFor 内不发）
    if (onSummary_) onSummary_(out);
    return out;
}

SummaryOutcome SummaryManager::summarizeWithVerdict(const std::vector<Msg>& material) const {
    SummaryOutcome out = generate(material);
    out.kind = SummaryKind::ContextCompaction;
    if (out.text.empty() && out.error.empty()) out.error = "摘要正文为空";
    out.ok = out.ok && !out.text.empty();
    if (!out.ok) {
        out.factProposals.clear();
        out.relationshipProposals.clear();
    }
    if (onSummary_) onSummary_(out);
    return out;
}

SummaryOutcome SummaryManager::generate(const std::vector<Msg>& material) const {
    SummaryRequest req;
    req.material = material;
    req.kind = SummaryKind::ContextCompaction;  // 兼容入口等价于上下文压缩
    req.source = "context_compaction";
    return generateFor(req);
}

std::string SummaryManager::systemPromptFor(SummaryKind kind,
                                            std::vector<std::string>* sentinels) const {
    const std::string limit = std::to_string(maxSummaryChars_ / 3);
    auto note = [&](const char* frag) {
        if (sentinels != nullptr) sentinels->push_back(frag);
    };
    switch (kind) {
    case SummaryKind::EpisodicMemory:
        note("你是 MIO 的经历摘要器");
        note("不推测、不补全、不编造");
        note("只输出摘要正文与上述标记行");
        return
            "你是 MIO 的经历摘要器。把下面这段对话经历整理成可长期召回的【经历摘要】。\n"
            "要求：\n"
            "1) 按时间顺序概括发生过的重要事件、认识的人、人物关系变化、"
            "用户明确陈述过的事实与偏好、未完成的约定；\n"
            "2) 只写对话中确实出现的内容，不推测、不补全、不编造；\n"
            "3) 不复述原文、不保留寒暄与闲聊细节；\n"
            "4) 用陈述句，不超过 " + limit + " 字；\n"
            "5) 摘要之后单独输出标记行（没有的项可省略）：\n"
            "话题：<十字以内概括主要话题>\n"
            "私密性判断：私密 或 公开\n"
            "事实提案：<predicate> | <object> | <置信度 0~1>\n"
            "关系提案：<predicate> | <object> | <置信度 0~1>\n"
            "其中事实提案/关系提案只写对话中明确出现的新信息，每条一行，没有就不写。\n"
            "只输出摘要正文与上述标记行，不要其他前缀或解释。";
    case SummaryKind::ContextCompaction:
        note("你是 MIO 的上下文压缩器");
        note("不添加新事实、不提出事实或关系提案");
        note("只输出压缩摘要正文与上述标记行");
        return
            "你是 MIO 的上下文压缩器。把下面这段对话压缩成一段可放回上下文的"
            "【压缩摘要】。\n"
            "要求：\n"
            "1) 保留仍然有效的目标、约束、已确认结论、未完成事项与关键实体；\n"
            "2) 不保留寒暄、重复内容与已经被推翻的结论；\n"
            "3) 只写对话中确实出现的内容，不添加新事实、不提出事实或关系提案；\n"
            "4) 用陈述句，不超过 " + limit + " 字；\n"
            "5) 摘要之后单独输出标记行（没有的可省略）：\n"
            "话题：<十字以内概括主要话题>\n"
            "私密性判断：私密 或 公开\n"
            "只输出压缩摘要正文与上述标记行，不要其他前缀或解释。";
    case SummaryKind::Manual:
        note("你是 MIO 的经历记录器");
        note("不添加未出现的内容");
        note("只输出正文与上述标记行");
        return
            "你是 MIO 的经历记录器。把下面的内容整理成一条可长期保存的"
            "【经历记忆】。\n"
            "要求：\n"
            "1) 按时间顺序概括关键事实、偏好与约定；不添加未出现的内容；\n"
            "2) 不超过 " + limit + " 字；\n"
            "3) 之后单独输出标记行（没有的可省略）：\n"
            "话题：<十字以内概括主要话题>\n"
            "私密性判断：私密 或 公开\n"
            "只输出正文与上述标记行。";
    }
    return "";
}

std::string SummaryManager::renderMaterial(const std::vector<Msg>& material) const {
    std::ostringstream s;
    for (const auto& src : material) {
        // 系统提示永不进入摘要材料
        if (src.role == Role::System) continue;
        // 用副本：清空 reasoningContent（reasoning 一律排除，不提供开关），
        // 也不改动调用方数据。
        Msg m = src;
        m.reasoningContent.clear();
        m.parts.clear();

        std::string line;
        switch (m.role) {
        case Role::System: continue;
        case Role::User: {
            line = "[用户";
            if (!m.senderName.empty()) line += ":" + cutUtf8Safe(m.senderName, 40);
            line += "] ";
            break;
        }
        case Role::Assistant: line = "[助手] "; break;
        case Role::Tool: line = "[工具回合] "; break;
        }
        if (m.role == Role::Tool) {
            // 工具结果正文可能夹带内部实现细节/凭据，不进入摘要材料；
            // 只保留"这里发生过一个工具回合"这一事实。
            line += "（工具结果正文不作为经历材料，仅供上下文压缩参考工具已执行）";
        } else {
            line += cutUtf8Safe(m.text, kMaterialLineBytes);
            if (!m.toolCalls.empty()) {
                // 只用工具名：完整 argumentsJson 绝不进入材料
                line += " | 调用工具:";
                for (const auto& tc : m.toolCalls) line += " " + tc.name;
            }
        }
        line += "\n";
        s << line;
    }
    return cutUtf8Safe(s.str(), kMaterialMaxBytes);
}

SummaryOutcome SummaryManager::generateFor(const SummaryRequest& request) const {
    SummaryOutcome out;
    out.kind = request.kind;
    // 私密性判定默认【私密】：模型没输出、输出无法解析、或调用失败时都保持
    // 最窄范围（未知/解析失败不得放宽可见性）。只有明确解析出"公开"才置 false。
    out.privateVerdict = true;
    if (!llm_) {
        out.error = "摘要模型不可用";
        return out;
    }

    std::vector<std::string> sentinels;
    const std::string systemPrompt = systemPromptFor(request.kind, &sentinels);

    ChatRequest req;
    req.systemPrompt = systemPrompt;
    Msg m{Role::User};
    m.text = renderMaterial(request.material);
    req.messages.push_back(std::move(m));

    std::string full;
    try {
        const ChatResponse resp = llm_->chat(req);
        full = resp.text;
        // resp.reasoning 一律忽略：它既不是正文，也不作为解析来源
    } catch (const std::exception& e) {
        out.error = std::string("摘要模型调用失败: ") + e.what();
        return out;
    } catch (...) {
        out.error = "摘要模型调用失败: 未知异常";
        return out;
    }

    if (full.empty()) {
        out.error = "摘要模型返回空正文";
        return out;
    }

    const std::string capped =
        cutUtf8Safe(full, static_cast<std::size_t>(maxSummaryChars_ * 2) + 32);

    // 只有经历摘要产出事实/关系提案；压缩与 manual 一律留空。
    const bool wantProposals = (request.kind == SummaryKind::EpisodicMemory);
    std::vector<RawProposal> rawFacts;
    std::vector<RawProposal> rawRelations;
    std::vector<std::string> body;

    std::istringstream in(capped);
    for (std::string raw; std::getline(in, raw);) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string line = trim(raw);
        if (line.empty()) {
            body.push_back(std::string());
            continue;
        }
        if (auto v = markerValue(line, "私密性判断")) {
            // 未输出/无法解析 → 保守判定为私密（"未知或解析失败保持最窄范围"）
            if (v->find("私密") != std::string::npos) out.privateVerdict = true;
            else if (v->find("公开") != std::string::npos) out.privateVerdict = false;
            else out.privateVerdict = true;
            continue;
        }
        if (auto v = markerValue(line, "话题")) {
            out.topic = cutUtf8Safe(trim(*v), kTopicBytes);
            continue;
        }
        if (auto v = markerValue(line, "事实提案")) {
            if (wantProposals) {
                RawProposal rp;
                if (parseRawProposal(*v, &rp)) rawFacts.push_back(rp);
            }
            continue;  // 非经历摘要：提案行同样不留在正文里
        }
        if (auto v = markerValue(line, "关系提案")) {
            if (wantProposals) {
                RawProposal rp;
                if (parseRawProposal(*v, &rp)) rawRelations.push_back(rp);
            }
            continue;
        }
        body.push_back(raw);
    }

    std::string bodyText = joinLines(body);
    while (!bodyText.empty() &&
           (bodyText.back() == '\n' || bodyText.back() == '\r' || bodyText.back() == ' '))
        bodyText.pop_back();

    const SanitizeResult clean = sanitizeSummaryText(bodyText, sentinels);
    if (!clean.ok) {
        out.error = clean.error;
        out.text.clear();
        return out;
    }
    out.text = clean.text;

    if (!wantProposals || out.text.empty()) {
        out.ok = !out.text.empty();
        if (!out.ok && out.error.empty()) out.error = "摘要正文为空";
        return out;
    }

    // 提案：模型只能提供 predicate/object/confidence；其余服务端填。
    // 没有参与者（服务端无法解析身份）时不产出提案 —— 绝不冒填 subjectId。
    if (!request.participants.empty()) {
        const std::string subjectId = request.participants.front();
        const std::string source =
            request.source.empty() ? std::string("silence_summary") : request.source;
        const std::string convKey = request.conversationKey;
        std::vector<std::string> evidence;
        if (!convKey.empty() && request.fromMessageId > 0)
            evidence.push_back(convKey + "#" + std::to_string(request.fromMessageId));
        if (!convKey.empty() && request.toMessageId > 0 &&
            request.toMessageId != request.fromMessageId)
            evidence.push_back(convKey + "#" + std::to_string(request.toMessageId));
        const std::int64_t createdAt = request.now > 0 ? request.now : 0;

        auto build = [&](const RawProposal& raw, ProposalKind kind) -> std::optional<Proposal> {
            // 提案字段同样过清洗器：凭据/提示词回显不得进入 proposal 层
            const SanitizeResult p = sanitizeSummaryText(raw.predicate, sentinels);
            const SanitizeResult o = sanitizeSummaryText(raw.object, sentinels);
            if (!p.ok || !o.ok) return std::nullopt;
            const std::string predicate = trim(p.text);
            const std::string object = trim(o.text);
            if (predicate.empty() || object.empty()) return std::nullopt;
            if (looksLikePlaceholder(predicate) || looksLikePlaceholder(object))
                return std::nullopt;
            Proposal prop;
            prop.kind = kind;
            // 服务端权威字段
            prop.subjectId = subjectId;
            prop.predicate = cutUtf8Safe(predicate, kProposalFieldBytes);
            prop.object = cutUtf8Safe(object, kProposalFieldBytes);
            prop.source = source;
            prop.conversationKey = convKey;
            prop.evidenceMessageIds = evidence;
            prop.confidence = raw.confidence;
            // 提案层状态：一律 pending / proposed / HUMAN_REVIEW_PENDING，
            // actor 空、reviewedAt 0、approvedBy 空（模型伪造的审核字段已丢弃）。
            prop.status = ProposalStatus::Pending;
            prop.factStatus = FactStatus::Proposed;
            prop.cognitionStatus = CognitionStatus::Inferred;
            prop.review.status = kHumanReviewPending;
            prop.review.actor.clear();
            prop.review.reason.clear();
            prop.review.reviewedAt = 0;
            prop.approvedBy.clear();
            // 摘要输出只可收窄：提案可见性取最窄（不得自动扩大为 public）
            prop.visibility = Visibility::Conversation;
            prop.createdAt = createdAt;
            return prop;
        };

        for (const auto& raw : rawFacts) {
            if (auto prop = build(raw, ProposalKind::Fact))
                out.factProposals.push_back(std::move(*prop));
        }
        for (const auto& raw : rawRelations) {
            if (auto prop = build(raw, ProposalKind::Relationship))
                out.relationshipProposals.push_back(std::move(*prop));
        }
    }

    out.ok = !out.text.empty();
    if (!out.ok && out.error.empty()) out.error = "摘要正文为空";
    return out;
}

} // namespace mio
