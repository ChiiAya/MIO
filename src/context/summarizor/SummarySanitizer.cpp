#include "context/summarizor/SummarySanitizer.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "core/contracts/Errors.h"

namespace mio {

namespace {

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 'A' && u <= 'Z') c = static_cast<char>(u - 'A' + 'a');
    }
    return out;
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                     s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

bool contains(const std::string& haystackLower, const std::string& needleLower) {
    return haystackLower.find(needleLower) != std::string::npos;
}

// 行级命中：返回命中的规则名（空 = 未命中）
std::string lineRuleHit(const std::string& lineLower,
                        const std::vector<std::string>& extraForbiddenLower) {
    // 1) 人工审核占位字段：这些是协议字段名，正文出现即为回显/伪造
    if (contains(lineLower, "human_review_")) return "human_review_placeholder";
    if (contains(lineLower, "reviewedat") || contains(lineLower, "reviewed_at"))
        return "human_review_timestamp_field";
    if (contains(lineLower, "approvedby") || contains(lineLower, "approved_by"))
        return "human_review_approver_field";
    // 2) reasoning 回显
    if (contains(lineLower, "reasoning_content") ||
        contains(lineLower, "reasoningcontent") ||
        contains(lineLower, "reasoning content") ||
        contains(lineLower, "chain of thought") ||
        contains(lineLower, "chain-of-thought"))
        return "reasoning_echo";
    // 中文标记不区分大小写，直接查原文（lower 后中文不变）
    if (contains(lineLower, "思维链") || contains(lineLower, "推理过程"))
        return "reasoning_echo";
    // 3) 凭据/密钥字样：整行删除（行内无法确定哪一段是密钥时最安全的做法）
    if (contains(lineLower, "api key") || contains(lineLower, "api-key") ||
        contains(lineLower, "apikey") || contains(lineLower, "api_key"))
        return "api_key_echo";
    if (contains(lineLower, "authorization") || contains(lineLower, "bearer ") ||
        contains(lineLower, "access token") || contains(lineLower, "access_token") ||
        contains(lineLower, "secret key") || contains(lineLower, "secret_key"))
        return "credential_echo";
    // 4) 调用方注册的系统提示片段回显
    for (const auto& frag : extraForbiddenLower) {
        if (!frag.empty() && contains(lineLower, frag)) return "system_prompt_echo";
    }
    return "";
}

// 行内剔除 sk-xxx 形式的密钥：保留行内容，只把密钥本体换成占位符。
// 返回是否发生剔除。
bool exciseInlineSecrets(std::string* line) {
    static const std::string kMarker = "sk-";
    bool changed = false;
    std::size_t pos = 0;
    while ((pos = line->find(kMarker, pos)) != std::string::npos) {
        std::size_t end = pos + kMarker.size();
        while (end < line->size()) {
            const unsigned char c = static_cast<unsigned char>((*line)[end]);
            const bool tokenChar = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                                   (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
            if (!tokenChar) break;
            ++end;
        }
        // 太短的 "sk-" 视为普通文本（避免误伤 "sk-8" 之类），长 token 才按密钥处理
        if (end - pos < kMarker.size() + 8) {
            pos = end;
            continue;
        }
        line->replace(pos, end - pos, "[已移除凭据]");
        pos += std::string("[已移除凭据]").size();
        changed = true;
    }
    return changed;
}

// 剔除密钥占位符后，行内是否还有实际内容？只有密钥（或密钥 + 标点）的行没有
// 保留价值 —— 留下来只会变成 "[已移除凭据]" 这样的空壳污染摘要。
bool lineHasMeaningAfterExcision(const std::string& line) {
    std::string s = line;
    const std::string placeholder = "[已移除凭据]";
    for (std::size_t p = s.find(placeholder); p != std::string::npos;
         p = s.find(placeholder, p)) {
        s.erase(p, placeholder.size());
    }
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == ' ' || u == '\t' || u == '\r' || u == '\n') continue;
        if (u < 0x80 && !std::isalnum(u)) continue;  // ASCII 标点/分隔符不算内容
        return true;
    }
    return false;
}

} // namespace

bool isValidUtf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        unsigned int cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return false;  // 续字节或非法首字节
        }
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // overlong / 代理区 / 超范围
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
        i += extra + 1;
    }
    return true;
}

std::string cutUtf8Safe(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

SanitizeResult sanitizeSummaryText(const std::string& text,
                                  const std::vector<std::string>& extraForbidden) {
    SanitizeResult out;
    if (text.empty()) {
        out.text.clear();
        return out;  // 空输入由调用方判定失败（清洗器不改变"空"的语义）
    }

    std::vector<std::string> forbiddenLower;
    forbiddenLower.reserve(extraForbidden.size());
    for (const auto& f : extraForbidden) forbiddenLower.push_back(toLowerAscii(f));

    std::istringstream in(text);
    std::vector<std::string> kept;
    bool modified = false;
    bool lastWasBlank = true;
    for (std::string raw; std::getline(in, raw);) {
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const std::string lower = toLowerAscii(raw);

        const std::string hit = lineRuleHit(lower, forbiddenLower);
        if (!hit.empty()) {
            modified = true;
            out.hits.push_back(hit);
            continue;  // 整行删除：不写入任何片段
        }
        std::string line = raw;
        if (exciseInlineSecrets(&line)) {
            modified = true;
            out.hits.push_back("inline_credential_excised");
            if (!lineHasMeaningAfterExcision(line)) {
                // 整行只有密钥：剔除后已是空壳，直接删除该行
                modified = true;
                out.hits.push_back("credential_only_line");
                continue;
            }
        }
        if (trim(line).empty()) {
            // 剔除后变成空行：折叠连续空行，保持正文可读
            if (lastWasBlank) {
                modified = true;
                continue;
            }
            kept.push_back(std::string());
            lastWasBlank = true;
            continue;
        }
        kept.push_back(line);
        lastWasBlank = false;
    }

    std::ostringstream joined;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i > 0) joined << '\n';
        joined << kept[i];
    }
    std::string cleaned = joined.str();
    while (!cleaned.empty() &&
           (cleaned.back() == '\n' || cleaned.back() == '\r' || cleaned.back() == ' '))
        cleaned.pop_back();

    out.modified = modified;
    if (trim(cleaned).empty()) {
        out.ok = false;
        out.text.clear();
        out.error = "摘要正文清洗后为空：命中禁止内容（";
        for (std::size_t i = 0; i < out.hits.size(); ++i) {
            if (i > 0) out.error += ",";
            out.error += out.hits[i];
        }
        out.error += "）";
        return out;
    }
    out.text = std::move(cleaned);
    return out;
}

} // namespace mio
