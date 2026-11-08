#include "mind/facts/Facts.h"

#include <chrono>
#include <ctime>
#include <sstream>

#include "context/costEstimator/Tokens.h"
#include "core/time/LocalTime.h"

namespace mio {

std::string nowTimeString() {
    return formatLocalTime(std::time(nullptr));
}

std::string buildSystemPrompt(const Persona& persona,
                              const RelationshipGraph& graph,
                              int maxPeopleTokens) {
    std::ostringstream s;
    if (!persona.systemPromptPrefix.empty()) {
        std::string prefix = persona.systemPromptPrefix;
        std::size_t pos = 0;
        while ((pos = prefix.find("{botName}", pos)) != std::string::npos) {
            prefix.replace(pos, 9, persona.botName);
            pos += persona.botName.length();
        }
        s << prefix;
        if (prefix.back() != '\n') s << "\n";
    } else {
        s << "你是 " << persona.botName << "，一位真实存在的、有自己性格的聊天伙伴。\n";
    }

    if (!persona.character.empty()) {
        s << persona.character;
        if (persona.character.back() != '\n') s << "\n";
    }

    // 认识的人（潜意识层，top-K，token 封顶）
    constexpr std::size_t kMaxPeople = 20;
    const auto people = graph.topKSnapshot(kMaxPeople);
    int peopleTokens = 0;
    for (const auto& p : people) {
        std::string line = "- " + p.name + " [internal_id:" + p.internalId + "]";
        const auto qq = RelationshipGraph::extractQq(p);
        if (!qq.empty()) line += " [qq:" + qq + "]";
        if (!p.personal.empty()) line += "（" + p.personal + "）";
        line += "\n";
        const int t = static_cast<int>(estimateTokens(line));
        if (peopleTokens + t > maxPeopleTokens) break;
        peopleTokens += t;
        s << line;
    }

    if (!persona.systemPromptNotice.empty()) {
        s << persona.systemPromptNotice;
        if (persona.systemPromptNotice.back() != '\n') s << "\n";
    }

    return s.str();
}

} // namespace mio
