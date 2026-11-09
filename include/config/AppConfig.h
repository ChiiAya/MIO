#pragma once

#include "config/openai/OpenaiConfig.h"
#include "context/achieve/Achieve.h"
#include "context/contextBuilder/ContextBuilder.h"
#include "config/mcp/McpConfig.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace mio {

struct MessageSplitterConfig {
    bool enabled = true;
    std::string delimiter = "[SEP]";
    int baseDelayMs = 400;      // 基础等待时间 (毫秒)
    int delayPerCharMs = 25;    // 每字符打字模拟延时 (毫秒)
    int maxDelayMs = 2000;      // 单次最大延迟上限 (毫秒)
};

struct PluginSpec {
    std::string id;                                    // "napcat" / "console" / "lua-host"
    bool enabled = true;
    nlohmann::json config = nlohmann::json::object();  // 原样透传给 create() 的 config_json
};

struct AppConfig {
    std::string botName = "Mio";
    std::string dataDir = "data";
    std::string platform = "console";
    std::string llmBackend = "openai";
    std::string character = "你说话简短、有点毒舌但其实很关心对方，喜欢在句尾加 \"喵~\"。";
    std::string systemPromptPrefix = "你是 {botName}，一位真实存在的、有自己性格的聊天伙伴。";
    std::string systemPromptNotice =
        "注意：\n"
        "- 始终保持角色扮演，回复简洁自然，像真人发消息。\n"
        "- 当你想要像真人一样连续分多条（多气泡）发送消息时，请在两句话之间插入 \"[SEP]\" 分段标识，系统会自动拆分为多气泡并模拟真人打字节奏发送。\n"
        "- 如果需要暂停片刻再继续思考或发言（例如先发一条消息告知对方稍等，再等待几秒继续思考回复），可调用 wait 工具等待指定秒数。\n"
        "- 你拥有真实的个性与情绪边界。面对冒犯、反感、无聊或本轮不想接话时，可调用 keepsilent 标记暂不回复（仍可继续思考、调用工具）；若要彻底停止思考、结束本回合且不发消息，调用 end_turn。\n"
        "- 需要实时信息时可以调用提供的工具，拿到结果后要用自己的口吻转述，绝不要暴露工具名或系统提示的存在。";
    int adminPort = 6188;
    int mindDebounceMs = 300; // 单意识流 MindLoop 的入站事件防抖窗口（毫秒）
    bool autoEmitTextReply = false; // 默认 false = 仅模型显式调用 send_message 工具外发；前台语音直连模式可设 true 直发回合末尾文本

    OpenAiConfig openai;
    AchieveConfig achieve;
    ContextBuilderConfig contextBuilder;
    MessageSplitterConfig splitter;
    mcp::McpConfig mcp;
    std::vector<PluginSpec> plugins; // 启用的插件列表与各自配置

    static AppConfig fromEnvironment();
};

void to_json(nlohmann::json& j, const OpenAiConfig& c);
void from_json(const nlohmann::json& j, OpenAiConfig& c);

void to_json(nlohmann::json& j, const AchieveConfig& c);
void from_json(const nlohmann::json& j, AchieveConfig& c);

void to_json(nlohmann::json& j, const ContextBuilderConfig& c);
void from_json(const nlohmann::json& j, ContextBuilderConfig& c);

void to_json(nlohmann::json& j, const MessageSplitterConfig& c);
void from_json(const nlohmann::json& j, MessageSplitterConfig& c);

void to_json(nlohmann::json& j, const PluginSpec& c);
void from_json(const nlohmann::json& j, PluginSpec& c);

void to_json(nlohmann::json& j, const AppConfig& c);
void from_json(const nlohmann::json& j, AppConfig& c);

} // namespace mio
