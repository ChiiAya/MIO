#include "core/contracts/Limits.h"

namespace mio {
namespace limits {

std::string truncateUtf8(const std::string& s, std::size_t maxBytes, bool* truncated) {
    if (truncated != nullptr) *truncated = false;
    if (s.size() <= maxBytes) return s;
    std::size_t cut = maxBytes;
    // 回退到某个 UTF-8 字符的起始字节，避免产生半个多字节序列
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    if (truncated != nullptr) *truncated = true;
    return s.substr(0, cut);
}

} // namespace limits
} // namespace mio
