#include "runtime/MindTools.h"

#include "context/achieve/Achieve.h"
#include "core/event/AttentionGate.h"
#include "core/event/Event.h"
#include "log/Log.h"
#include "mind/facts/Facts.h"
#include "providers/llm/tool/ToolRegistry.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace mio {

namespace {

constexpr const char* kTag = "MindTools";

double parseWaitSeconds(const nlohmann::json& args) {
    auto parseVal = [](const nlohmann::json& val, double& out) -> bool {
        if (val.is_number()) {
            out = val.get<double>();
            return true;
        }
        if (val.is_string()) {
            try {
                out = std::stod(val.get<std::string>());
                return true;
            } catch (...) {}
        }
        return false;
    };

    double seconds = 0.0;
    if (args.contains("seconds")) {
        parseVal(args["seconds"], seconds);
    } else if (args.contains("duration")) {
        parseVal(args["duration"], seconds);
    }
    return std::clamp(seconds < 0.1 ? 0.5 : seconds, 0.5, 60.0);
}

} // namespace

void registerMindTools(ToolRegistry& registry, MindToolsDeps deps) {
    // 1. get_current_time
    ToolDef timeDef;
    timeDef.name = "get_current_time";
    timeDef.description = "获取当前的本地日期与时间（精确到分钟）。";
    timeDef.parametersJsonSchema = {{"type", "object"},
                                    {"properties", nlohmann::json::object()}};
    registry.add(std::move(timeDef),
                 [](const nlohmann::json&) -> std::string {
                     return "当前时间: " + nowTimeString();
                 },
                 ToolLayer::Builtin);

    // 2. keepsilent (+ keep_silent / stay_silent 别名)
    const nlohmann::json silentSchema = {
        {"type", "object"},
        {"properties",
         {{"reason",
           {{"type", "string"},
            {"description", "决定保持沉默的内心理由（如：被惹毛了不想说话/觉得对方很无聊）"}}}}},
        {"required", nlohmann::json::array({"reason"})}};

    for (const char* name : {"keepsilent", "keep_silent", "stay_silent"}) {
        ToolDef def;
        def.name = name;
        def.description = (std::string(name) == "keepsilent")
            ? "当你阅读了用户的话后，内心感到被冒犯不想回复、想要已读不回、或者认为此时保持沉默更符合你的心情时，调用此工具保持沉默。调用后你不会向用户发送任何消息。"
            : "同 keepsilent，在不想回答或决定已读不回时调用。";
        def.parametersJsonSchema = silentSchema;
        def.isTerminal = true;

        auto markSilent = deps.markKeepSilent;
        registry.add(
            std::move(def),
            [markSilent = std::move(markSilent), toolName = std::string(name)](const nlohmann::json& args) -> std::string {
                if (markSilent) markSilent();
                log::info(kTag, "模型调用 " + toolName + " 保持沉默: " + args.value("reason", ""));
                return "已选择保持沉默。本轮不会向用户发送任何回复。";
            },
            ToolLayer::Builtin);
    }

    // 3. wait (+ sleep 别名)
    const nlohmann::json waitSchema = {
        {"type", "object"},
        {"properties",
         {{"seconds",
           {{"type", "number"},
            {"description", "需要等待的秒数（支持整数或小数，例如 3 或 5.0，建议 1~30 秒，最大不超过 60 秒）"}}},
          {"duration",
           {{"type", "number"},
            {"description", "同 seconds，需要等待的秒数（备用字段）"}}},
          {"reason",
           {{"type", "string"},
            {"description", "等待的原因或打算下一步做的事情（可选）"}}}}},
        {"required", nlohmann::json::array({"seconds"})}};

    auto getGen = deps.currentGeneration;
    auto waitHandler = [getGen](const nlohmann::json& args) -> std::string {
        const double seconds = parseWaitSeconds(args);
        const std::string reason = args.value("reason", "");
        log::info(kTag, "模型调用 wait: 等待 " + std::to_string(seconds) + " 秒" +
                            (reason.empty() ? "" : (", 理由=" + reason)));

        const int totalMs = static_cast<int>(seconds * 1000.0);
        constexpr int kSliceMs = 100;
        int elapsedMs = 0;
        const std::uint64_t initialGen = getGen ? getGen() : 0;
        while (elapsedMs < totalMs) {
            if (getGen && getGen() != initialGen) {
                log::info(kTag, "wait 过程中收到新事件打断，提前结束等待");
                break;
            }
            const int slice = std::min(kSliceMs, totalMs - elapsedMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            elapsedMs += slice;
        }

        std::ostringstream oss;
        oss << "已等待 " << std::fixed << std::setprecision(1) << seconds
            << " 秒。等待已结束，当前时间：" << nowTimeString()
            << "。你可以继续思考并决定是否调用工具或向用户发送消息。";
        return oss.str();
    };

    for (const char* name : {"wait", "sleep"}) {
        ToolDef def;
        def.name = name;
        def.description = (std::string(name) == "wait")
            ? "让系统等待/暂停指定的时长（秒），等待结束后会再次唤醒你继续思考和采取下一步行动。适用于需要稍作停顿再继续发言、给用户留出反应阅读时间、或者先发消息告知对方稍等后再进行查阅思考的场景。"
            : "同 wait，等待指定时长后再次继续思考和行动。";
        def.parametersJsonSchema = waitSchema;
        registry.add(std::move(def), waitHandler, ToolLayer::Builtin);
    }

    // 4. query_events —— 从 Achieve 全文事件库检索完整详情
    ToolDef queryEvDef;
    queryEvDef.name = "query_events";
    queryEvDef.description =
        "从事件档案库（Achieve）按时间范围、最近时长（秒）或事件 ID 查询完整事件详情。"
        "当你在上下文中只看到 [Preview] 或 [Digest] 摘要而想查看具体消息正文时调用。";
    queryEvDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"target",
           {{"type", "string"},
            {"description", "可选过滤目标：source（如 napcat）、channelId（如 group:123 或 private:456）或 actorId；留空查全部"}}},
          {"recent_seconds",
           {{"type", "integer"},
            {"description", "查询最近 N 秒内的事件（如 300 表示最近 5 分钟）"}}},
          {"start_time",
           {{"type", "integer"},
            {"description", "起始 Unix 时间戳（秒或毫秒，配合 end_time 使用）"}}},
          {"end_time",
           {{"type", "integer"},
            {"description", "结束 Unix 时间戳（秒或毫秒，0 表示当前时间）"}}},
          {"event_id",
           {{"type", "integer"},
            {"description", "按单个事件序号 #id 精确查询"}}},
          {"limit",
           {{"type", "integer"},
            {"description", "最大返回条数（默认 30，上限 100）"}}}}}};
    Achieve* achievePtr = &deps.achieve;
    registry.add(
        std::move(queryEvDef),
        [achievePtr](const nlohmann::json& args) -> std::string {
            if (args.contains("event_id") && args["event_id"].is_number_integer()) {
                const auto id = args["event_id"].get<std::uint64_t>();
                if (auto ev = achievePtr->getById(id)) {
                    return Achieve::formatEventsForTool({*ev});
                }
                return "未找到事件 #" + std::to_string(id);
            }
            const std::string target = args.value("target", "");
            const auto limit = static_cast<std::size_t>(std::clamp(args.value("limit", 30), 1, 100));
            if (args.contains("recent_seconds") && args["recent_seconds"].is_number_integer()) {
                const auto sec = args["recent_seconds"].get<std::int64_t>();
                return Achieve::formatEventsForTool(achievePtr->queryRecentSeconds(target, sec, limit));
            }
            if (args.contains("start_time") && args["start_time"].is_number_integer()) {
                const auto start = args["start_time"].get<std::int64_t>();
                const auto end = args.value("end_time", std::int64_t{0});
                return Achieve::formatEventsForTool(achievePtr->queryByTime(target, start, end, limit));
            }
            return Achieve::formatEventsForTool(achievePtr->queryRecentSeconds(target, 600, limit));
        },
        ToolLayer::Builtin);

    // 5. set_attention_rule / list_attention_rules
    AttentionGate* gatePtr = &deps.gate;
    ToolDef setRuleDef;
    setRuleDef.name = "set_attention_rule";
    setRuleDef.description =
        "设置注意力门规则，调整特定对象（pattern，如 group:123、private:456、actor:789、napcat）的注意力层级："
        "DirectFull（直接全文唤醒）、Preview（标题预览唤醒）、Digest（攒够一批再聚合唤醒）、Silent（静默归档不打扰）。";
    setRuleDef.parametersJsonSchema = {
        {"type", "object"},
        {"properties",
         {{"pattern",
           {{"type", "string"},
            {"description", "匹配模式，如 group:123456、user:789、source:napcat"}}},
          {"tier",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"DirectFull", "Preview", "Digest", "Silent"})},
            {"description", "目标注意力层级"}}},
          {"duration_seconds",
           {{"type", "integer"},
            {"description", "规则生效时长（秒，0 或不填表示长期生效）"}}},
          {"reason",
           {{"type", "string"},
            {"description", "设置该规则的原因"}}}}},
        {"required", nlohmann::json::array({"pattern", "tier"})}};
    registry.add(
        std::move(setRuleDef),
        [gatePtr](const nlohmann::json& args) -> std::string {
            const std::string pattern = args.value("pattern", "");
            const std::string tierStr = args.value("tier", "Preview");
            if (pattern.empty()) return "error: pattern 不能为空";
            auto tier = attentionTierFromString(tierStr);
            if (!tier) return "error: 无效的 tier: " + tierStr;

            AttentionRule rule;
            rule.pattern = pattern;
            rule.targetTier = *tier;
            rule.reason = args.value("reason", "");
            const std::int64_t dur = args.value("duration_seconds", std::int64_t{0});
            if (dur > 0) {
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                rule.expiresAt = nowMs + dur * 1000;
            }
            gatePtr->upsertRule(std::move(rule));
            return "已设置注意力规则: " + pattern + " -> " + attentionTierToString(*tier);
        },
        ToolLayer::Builtin);

    ToolDef listRuleDef;
    listRuleDef.name = "list_attention_rules";
    listRuleDef.description = "列出当前生效的所有注意力门规则。";
    listRuleDef.parametersJsonSchema = {{"type", "object"},
                                        {"properties", nlohmann::json::object()}};
    registry.add(
        std::move(listRuleDef),
        [gatePtr](const nlohmann::json&) -> std::string {
            const auto rules = gatePtr->listRules();
            if (rules.empty()) return "当前无自定义注意力规则。";
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& r : rules) {
                arr.push_back({{"pattern", r.pattern},
                               {"tier", attentionTierToString(r.targetTier)},
                               {"expiresAt", r.expiresAt},
                               {"reason", r.reason}});
            }
            return arr.dump();
        },
        ToolLayer::Builtin);
}

} // namespace mio

