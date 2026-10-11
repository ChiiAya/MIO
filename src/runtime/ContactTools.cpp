#include "runtime/ContactTools.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <optional>
#include <stdexcept>

#include "mind/graph/RelationshipGraph.h"
#include "providers/llm/tool/ToolRegistry.h"

namespace mio {
namespace {
using Json = nlohmann::json;

std::string stringArg(const Json& args, const char* key, bool required = false) {
    if (!args.contains(key)) {
        if (required) throw std::invalid_argument(std::string("缺少参数: ") + key);
        return {};
    }
    if (!args.at(key).is_string())
        throw std::invalid_argument(std::string("参数必须为字符串: ") + key);
    auto value = args.at(key).get<std::string>();
    const auto first = value.find_first_not_of(" \t\r\n");
    value = first == std::string::npos ? "" :
        value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    if (required && value.empty())
        throw std::invalid_argument(std::string("参数不能为空: ") + key);
    return value;
}

Json contactJson(const PersonNode& node) {
    return {{"internal_id", node.internalId}, {"name", node.name},
            {"qq", RelationshipGraph::extractQq(node)},
            {"platform_ids", node.platformIds}, {"notes", node.personal}};
}

Json textField(const char* description) {
    return {{"type", "string"}, {"description", description}};
}
} // namespace

void registerContactTools(ToolRegistry& registry, RelationshipGraph& graph,
                          std::function<void()> onChanged) {
    ToolDef list;
    list.name = "list_contacts";
    list.description = "查询通讯录。按称呼、QQ 或 internal_id 查找认识的人；不填 query 则列出全部联系人。重名时通过 internal_id 区分。";
    list.parametersJsonSchema = {{"type", "object"}, {"properties", {
        {"query", textField("可选的称呼、QQ 或 internal_id")}}}};
    registry.add(std::move(list), [&graph](const Json& args) {
        const auto query = stringArg(args, "query");
        Json out = Json::array();
        for (const auto& node : graph.contacts()) {
            bool match = query.empty() || node.internalId == query ||
                node.name.find(query) != std::string::npos ||
                RelationshipGraph::extractQq(node) == query;
            for (const auto& id : node.platformIds) match = match || id == query;
            if (match) out.push_back(contactJson(node));
        }
        return out.dump();
    });

    ToolDef upsert;
    upsert.name = "upsert_contact";
    upsert.description = "将新认识的人加入通讯录或更新已有联系人的称呼、备注。只记录对话中明确给出的信息，不猜测账号。QQ 用户可传 qq；其他平台同时传 platform 和 user_id。暂不知道账号时可以只记录 name。";
    upsert.parametersJsonSchema = {{"type", "object"}, {"properties", {
        {"name", textField("联系人的称呼")},
        {"qq", textField("可选的 QQ 号，仅数字")},
        {"platform", textField("其他平台标识，与 user_id 同时提供")},
        {"user_id", textField("平台账号，与 platform 同时提供")},
        {"notes", textField("可选的特点、喜好或印象，省略时保留已有备注")}}},
        {"required", Json::array({"name"})}};
    registry.add(std::move(upsert), [&graph, onChanged](const Json& args) {
        const auto name = stringArg(args, "name", true);
        auto platform = stringArg(args, "platform");
        auto userId = stringArg(args, "user_id");
        const auto qq = stringArg(args, "qq");
        if (!qq.empty()) {
            if ((!platform.empty() && platform != "qq" && platform != "napcat" && platform != "onebot") ||
                (!userId.empty() && userId != qq))
                throw std::invalid_argument("qq 与 platform/user_id 冲突");
            platform = "qq";
            userId = qq;
        }
        if (platform.empty() != userId.empty())
            throw std::invalid_argument("platform 与 user_id 必须同时提供");
        if ((platform == "qq" || platform == "napcat" || platform == "onebot") &&
            !std::all_of(userId.begin(), userId.end(), [](unsigned char ch) { return std::isdigit(ch); }))
            throw std::invalid_argument("QQ 号必须仅包含数字");
        std::optional<std::string> notes;
        if (args.contains("notes")) notes = stringArg(args, "notes");
        auto result = graph.upsertContact(name, platform, userId, notes, std::time(nullptr));
        if (onChanged) onChanged();
        return contactJson(result).dump();
    });

    ToolDef nickname;
    nickname.name = "set_nickname";
    nickname.description = "更新已知联系人的称呼。current 支持 internal_id、QQ 或唯一称呼；重名时必须使用账号或 internal_id。";
    nickname.parametersJsonSchema = {{"type", "object"}, {"properties", {
        {"current", textField("联系人 internal_id、QQ 或当前称呼")},
        {"nickname", textField("新的称呼")}}},
        {"required", Json::array({"current", "nickname"})}};
    registry.add(std::move(nickname), [&graph, onChanged](const Json& args) {
        auto result = graph.updateContact(stringArg(args, "current", true),
            stringArg(args, "nickname", true), std::nullopt);
        if (onChanged) onChanged();
        return contactJson(result).dump();
    });

    ToolDef notes;
    notes.name = "set_notes";
    notes.description = "更新已知联系人的特点、喜好或印象。person 支持 internal_id、QQ 或唯一称呼；会替换此前备注。";
    notes.parametersJsonSchema = {{"type", "object"}, {"properties", {
        {"person", textField("联系人 internal_id、QQ 或唯一称呼")},
        {"notes", textField("完整的新备注，空字符串可清除备注")}}},
        {"required", Json::array({"person", "notes"})}};
    registry.add(std::move(notes), [&graph, onChanged](const Json& args) {
        if (!args.contains("notes")) throw std::invalid_argument("缺少参数: notes");
        auto result = graph.updateContact(stringArg(args, "person", true),
            std::nullopt, stringArg(args, "notes"));
        if (onChanged) onChanged();
        return contactJson(result).dump();
    });
}
} // namespace mio
