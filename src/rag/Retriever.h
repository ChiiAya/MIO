#pragma once
// ============================================================================
// RAG 预留接口：未来记忆系统（图搜索）的接入点。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace mio {

struct RetrievedEvent {
    std::uint64_t seq;
    std::string conversation;
    std::string senderId;
    std::string text;
    std::int64_t createdAt;
};

class Retriever {
public:
    virtual ~Retriever() = default;
    virtual std::vector<RetrievedEvent> retrieve(const std::string& query,
                                                 std::size_t topK = 5) = 0;
};

} // namespace mio
