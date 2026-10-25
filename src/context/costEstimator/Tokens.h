#pragma once
// ============================================================================
// Token 估算与 UTF-8 工具 —— 只用于阈值判断，不追求精确
//
// 用途：历史压缩触发阈值，误差 ±20% 无关紧要；精确账单看响应里的 Usage 字段。
// 估算规则：中文大部分场景 1 字 ≈ 1 token；英文/数字平均 4 字符 ≈ 1 token。
// 混合文本按 UTF-8 码点逐个归类粗算即可。
// ============================================================================

#include <algorithm>
#include <string>
#include <vector>

#include "core/message/Message.h"

namespace mio {

inline std::size_t decodeUtf8Point(const std::string& s, std::size_t i,
                                   std::uint32_t& outCp) {
    const auto lead = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    std::uint32_t cp = 0;
    if (lead < 0x80) {
        cp = lead;
    } else if ((lead >> 5) == 0x6 && i + 1 < s.size()) { // 110xxxxx
        cp = lead & 0x1F;
        len = 2;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    } else if ((lead >> 4) == 0xE && i + 2 < s.size()) { // 1110xxxx
        cp = lead & 0x0F;
        len = 3;
        for (int k = 1; k <= 2; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    } else if ((lead >> 3) == 0x1E && i + 3 < s.size()) { // 11110xxx
        cp = lead & 0x07;
        len = 4;
        for (int k = 1; k <= 3; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    } else {
        cp = 0xFFFD; // 非法序列，按替换字符处理
    }
    outCp = cp;
    return len;
}

inline bool isWideChar(std::uint32_t cp) {
    // CJK 统一表意文字及常用全角区段；不需要完备，够用即可
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x30000 && cp <= 0x3134F);
}

inline std::int64_t estimateTokens(const std::string& text) {
    std::int64_t wide = 0, narrow = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::uint32_t cp = 0;
        i += decodeUtf8Point(text, i, cp);
        if (isWideChar(cp))
            ++wide;
        else if (!std::isspace(static_cast<int>(cp)) && cp < 128)
            ++narrow;
    }
    return wide + (narrow + 3) / 4;
}

inline std::int64_t estimateTokens(const Msg& m) {
    std::int64_t t = estimateTokens(m.text) + estimateTokens(m.reasoningContent);
    for (const auto& p : m.parts) {
        // 图片 part 的 text 是 base64 data-uri，按文本估会得出天量 token，
        // 直接触发"上下文超限 -> 压缩"。这里按一次视觉输入粗略计费。
        if (p.kind == Part::Kind::Image) {
            t += 1024;
            continue;
        }
        t += estimateTokens(p.text);
    }
    return t;
}

inline std::int64_t estimateTokens(const std::vector<Msg>& msgs) {
    std::int64_t total = 0;
    for (const auto& m : msgs) {
        total += estimateTokens(m);
        for (const auto& tc : m.toolCalls)
            total += estimateTokens(tc.name + tc.argumentsJson);
    }
    return total;
}

// 按 maxBytes 截断字符串，保证不把 UTF-8 多字节字符截成半截。
// 工具结果预览、错误信息截短都会用到（跨过字节边界会产生乱码）。
inline std::string cutUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut; // 回退到某个字符的起始字节
    return s.substr(0, cut);
}

} // namespace mio
