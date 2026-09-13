#include "providers/llm/tool/ToolLoop.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "context/costEstimator/Tokens.h"
#include "log/Log.h"

namespace mio {

namespace {

// 护栏 0：参数白名单过滤。
// 只放行 JSON Schema properties 里声明过的键。小模型会幻觉出 extra_args、
// time_zone 等不存在的参数；不过滤轻则脏日志，重则下游误用。
nlohmann::json filterArgs(const ToolDef& def, const nlohmann::json& raw) {
    const auto props = def.parametersJsonSchema.find("properties");
    if (props == def.parametersJsonSchema.end() || !props->is_object()) return {};
    nlohmann::json out = nlohmann::json::object();
    for (auto it = raw.begin(); it != raw.end(); ++it)
        if (props->contains(it.key())) out[it.key()] = it.value();
    return out;
}

std::string sanitizeForFilename(const std::string& s) {
    std::string out;
    for (char c : s)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
                   ? c
                   : '_';
    return out.empty() ? "call" : out;
}

struct ExecOutcome {
    std::string resultText; // 进入 role=tool 消息的最终文本（已含护栏修饰）
};

// 执行一次工具调用，所有护栏集中在这里
ExecOutcome executeOne(ToolRegistry& reg, const ToolCall& call,
                       const ToolLoopOptions& opt,
                       std::pair<std::string, int>& streakState) {
    std::string result;

    // 护栏 1：未知工具名 -> 不中断，把可用名单教给模型让它自纠
    const auto def = reg.find(call.name);
    if (!def.has_value()) {
        auto names = reg.names();
        std::ostringstream msg;
        msg << "error: 工具 '" << call.name << "' 不存在。可用工具: ";
        for (std::size_t i = 0; i < names.size(); ++i)
            msg << (i ? ", " : "") << names[i];
        log::info("ToolLoop", "工具调用失败: " + call.name + " 不存在");
        return {msg.str()};
    }

    // argumentsJson 是模型手写的 JSON —— 解析失败就把错误回喂让它重试
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(
            call.argumentsJson.empty() ? "{}" : call.argumentsJson);
        if (!args.is_object()) throw std::runtime_error("参数不是 JSON 对象");
    } catch (const std::exception& e) {
        std::ostringstream msg;
        msg << "error: 参数 JSON 解析失败(" << e.what()
            << ")。请重新调用，arguments 必须是合法的 JSON 对象字符串";
        log::info("ToolLoop",
                  "工具调用失败: " + call.name +
                      " 参数解析失败: " + e.what());
        return {msg.str()};
    }

    nlohmann::json valid = filterArgs(*def, args);

    // 过滤掉的参数打出来 —— 调试小模型行为时这条日志价值极高
    if (valid.size() != args.size())
        log::warn("ToolLoop", "工具=" + call.name + " 丢弃未声明参数" +
                                  std::to_string(args.size() - valid.size()) +
                                  "个");

    try {
        result = reg.invoke(call.name, valid);
    } catch (const std::exception& e) {
        result = "error: 工具执行异常: " + std::string(e.what());
    }

    // 模型参数可能含非法 UTF-8（例如 topic 被小模型编坏），dump 时必须替换
    // 而不是抛异常；否则工具循环会被调试日志本身打断。
    const std::string argsDump =
        valid.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    log::info("ToolLoop",
              "工具调用: " + call.name +
                  " id=" + call.id +
                  " args=" + argsDump +
                  " result=" + cutUtf8(result, 300));

    // 护栏 2：同名同参连续调用检测
    const std::string key = call.name + "|" + argsDump;
    if (key == streakState.first)
        ++streakState.second;
    else
        streakState = {key, 1};
    if (streakState.second >= opt.repeatStreakThreshold && def.has_value() &&
        result.rfind("error:", 0) != 0) {
        result += "\n\n[SYSTEM NOTICE] 你已经连续 " +
                  std::to_string(streakState.second) +
                  " 次以相同参数调用同一工具。除非每次都能获得新信息，"
                  "否则请换一种做法或直接基于已知信息回复用户。";
    }

    // 护栏 3：超大结果落盘截断
    if (result.size() > opt.maxInlineResultBytes) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(opt.overflowDir, ec);
        const std::string name =
            sanitizeForFilename(call.id.empty() ? call.name + "_result" : call.id);
        const fs::path file = opt.overflowDir / (name + ".txt");
        std::ofstream of(file, std::ios::trunc);
        of << result;
        const std::string preview =
            cutUtf8(result, opt.maxInlineResultBytes / 2) + "\n...[截断]";
        result = preview + "\n[SYSTEM NOTICE] 结果过大，完整内容已写入: " +
                 file.string() + "。请使用更窄的查询条件获取精确结果。";
    }
    return {result};
}

} // namespace

ChatResponse runToolLoop(Llm& llm, ToolRegistry& reg, ChatRequest req,
                         const ToolLoopOptions& opt, const MessageSink& sink) {
    ChatResponse last;
    Usage totalUsage;

    for (int step = 1; step <= opt.maxSteps; ++step) {
        ChatResponse resp;
        bool gotEmpty = false;

        // 护栏 4：空输出短重试（连续 2 次空输出则报错）
        for (int attempt = 0; attempt < 2; ++attempt) {
            resp = llm.chat(req);
            if (!resp.text.empty() || !resp.toolCalls.empty()) {
                gotEmpty = false;
                break;
            }
            gotEmpty = true;
            log::warn("ToolLoop",
                      "第" + std::to_string(attempt + 1) + "次得到空输出，重试");
        }
        if (gotEmpty)
            throw std::runtime_error("模型连续返回空输出（无文本也无工具调用）");
        last = resp;
        totalUsage.inputOther += resp.usage.inputOther;
        totalUsage.inputCached += resp.usage.inputCached;
        totalUsage.output += resp.usage.output;

        if (resp.toolCalls.empty()) {
            Msg done{Role::Assistant};
            done.text = resp.text;
            done.reasoningContent = resp.reasoning;
            req.messages.push_back(done);
            if (sink) sink(done);
            last.usage = totalUsage;
            return last; // 正常收尾
        }

        // 先固化 assistant 的 tool_calls 消息（协议要求：结果之前必须先有调用记录）
        Msg assistant{Role::Assistant};
        assistant.text = resp.text;
        assistant.reasoningContent = resp.reasoning;
        assistant.toolCalls = resp.toolCalls;
        req.messages.push_back(assistant);
        if (sink) sink(assistant);

        std::string names;
        for (const auto& tc : resp.toolCalls) names += " " + tc.name;
        log::info("ToolLoop", "step=" + std::to_string(step) + " 调用工具:" + names);

        bool shouldTerminate = false;
        std::pair<std::string, int> streak{"", 0};
        for (const auto& tc : resp.toolCalls) {
            auto def = reg.find(tc.name);
            if (def.has_value() && def->isTerminal) {
                shouldTerminate = true;
            }
            ExecOutcome out = executeOne(reg, tc, opt, streak);
            log::debug("ToolLoop", tc.name + " => " + cutUtf8(out.resultText, 160));
            Msg result{Role::Tool};
            result.toolCallId = tc.id;
            result.text = out.resultText;
            req.messages.push_back(result);
            if (sink) sink(result);
        }

        if (shouldTerminate) {
            log::info("ToolLoop", "检测到终结性工具调用 (如 keepsilent)，提前结束工具循环");
            last.text.clear();
            last.usage = totalUsage;
            return last;
        }
    }

    // 护栏 5：步数耗尽 -> 拔掉工具 + 注入强制总结指令再走一步。
    // 这意味着哪怕任务没做完，用户也一定会收到一句人话答复，而不是沉默或报错。
    // （先清空工具再注入提示词）
    log::info("ToolLoop", "达到最大步数，强制总结收尾");
    req.tools.clear();
    Msg notice{Role::User};
    // 这条通知【不】标 ephemeral：它应该进入历史，否则模型下一轮看到自己
    // 收到过一条凭空消失的用户消息（作为普通 user 消息持久化）
    notice.text =
        "(系统提示) 已达到工具调用步数上限。请停止调用任何工具，"
        "基于以上已获得的信息，直接、简短地回复用户。";
    req.messages.push_back(notice);
    if (sink) sink(notice);
    last = llm.chat(req);
    totalUsage.inputOther += last.usage.inputOther;
    totalUsage.inputCached += last.usage.inputCached;
    totalUsage.output += last.usage.output;
    last.usage = totalUsage;
    return last;
}

} // namespace mio
