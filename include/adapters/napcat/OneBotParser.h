#pragma once

#include "adapters/napcat/OneBotTypes.h"
#include "core/event/Event.h"

#include <optional>
#include <string>

namespace mio {
namespace napcat {

OneBotEvent parseOneBotEvent(const nlohmann::json& raw);

// 将 OneBotEvent 转换为 MIO 统一 Event（支持 message 以及 notice/request）
std::optional<Event> oneBotEventToMioEvent(const OneBotEvent& ev);

std::string describeOneBotEvent(const OneBotEvent& ev);

} // namespace napcat
} // namespace mio
