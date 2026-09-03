#pragma once
// ============================================================================
// OneBot 11 / NapCat 事件解析器
//
// 边界：
//   * OneBotParser 只负责 JSON -> 结构化事件 / IncomingMessage；
//   * 不持有任何连接状态，不调用 Runtime；
//   * message 事件才返回 IncomingMessage；notice/request/meta 事件统一进
//     OneBotEvent 供上层记录或未来消费。
// ============================================================================

#include "OneBotTypes.h"

#include "core/event/Event.h"

#include <optional>
#include <string>

namespace mio {
namespace napcat {

// 解析任意 OneBot JSON 帧；不会抛异常，解析失败返回 postType=Unknown
OneBotEvent parseOneBotEvent(const nlohmann::json& raw);

// message 事件 -> Runtime IncomingMessage；非消息/无正文/机器人自身事件返回 nullopt
std::optional<IncomingMessage> eventToIncomingMessage(const OneBotEvent& ev);

// 供日志/调试使用：输出事件类型与关键 id
std::string describeOneBotEvent(const OneBotEvent& ev);

} // namespace napcat
} // namespace mio
