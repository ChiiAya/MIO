#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
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
    } else if ((lead >> 5) == 0x6 && i + 1 < s.size()) {
        cp = lead & 0x1F;
        len = 2;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    } else if ((lead >> 4) == 0xE && i + 2 < s.size()) {
        cp = lead & 0x0F;
        len = 3;
        for (int k = 1; k <= 2; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    } else if ((lead >> 3) == 0x1E && i + 3 < s.size()) {
        cp = lead & 0x07;
        len = 4;
        for (int k = 1; k <= 3; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    } else {
        cp = 0xFFFD;
    }
    outCp = cp;
    return len;
}

inline bool isWideChar(std::uint32_t cp) {
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
    t += static_cast<std::int64_t>(m.parts.size()) * 1024;
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

inline std::string cutUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut;
    return s.substr(0, cut);
}

} // namespace mio

