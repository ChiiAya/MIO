// ============================================================================
// deepseek-search：调用 DeepSeek 官方 Search / Fetch 工具 API 的原生插件
//
//   POST {baseUrl}/v1/tools/search  -> 注册 LLM 工具 web_search
//   POST {baseUrl}/v1/tools/fetch   -> 注册 LLM 工具 web_fetch
//
// 文档：
//   https://api-docs.deepseek.com/zh-cn/api/search.md
//   https://api-docs.deepseek.com/zh-cn/api/fetch.md
//
// 计费：按成功请求次数计费（Search / Fetch 各 5 元 / 1000 次），失败与空结果不计费。
// 鉴权：与 DeepSeek API 相同的 Bearer Key（项目里 openai.apiKey 如果是别的厂商，
//       这里需要单独填 DeepSeek 的 Key，或设置环境变量）。
// ============================================================================
#include "mio/plugin.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

constexpr const char* kTag = "deepseek-search";
constexpr const char* kSearchPath = "/v1/tools/search";
constexpr const char* kFetchPath = "/v1/tools/fetch";

// ---------------------------------------------------------------------------
// 配置（config.json -> plugins[].config）
// ---------------------------------------------------------------------------
struct SearchConfig {
    std::string baseUrl = "https://api.deepseek.com";
    std::string apiKey;                    // 直填 Key；留空则读 apiKeyEnv 环境变量
    std::string apiKeyEnv = "DEEPSEEK_API_KEY";
    int timeoutMs = 30000;                 // 单次 HTTP 超时
    int defaultCount = 5;                  // web_search 未传 count 时的默认条数
    int maxSummaryBytes = 720;             // 每条摘要最多字节（UTF-8 安全截断）
    int maxContentBytes = 2400;            // need_content=true 时每条正文最多字节
    int maxFetchBytes = 3800;              // web_fetch 正文最多字节
    int maxResultBytes = 3800;             // web_search 单次输出总字节预算
};

int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// UTF-8 安全截断：不把多字节字符切成乱码
std::string cutUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// 模型手写参数、上游 JSON 都可能是 null / 错类型；统一安全取值，绝不让异常冒到 ToolLoop
std::string jsonStr(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

int jsonInt(const nlohmann::json& obj, const char* key, int fallback) {
    const auto it = obj.find(key);
    if (it != obj.end()) {
        if (it->is_number_integer()) return it->get<int>();
        if (it->is_number_float()) return static_cast<int>(it->get<double>());
    }
    return fallback;
}

bool jsonBool(const nlohmann::json& obj, const char* key, bool fallback) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

SearchConfig parseConfig(const nlohmann::json& cfg) {
    SearchConfig c;
    if (!cfg.is_object()) return c;
    auto str = [&cfg](const char* key, const std::string& fallback) {
        if (cfg.contains(key) && cfg[key].is_string()) return cfg[key].get<std::string>();
        return fallback;
    };
    auto num = [&cfg](const char* key, int fallback) {
        if (cfg.contains(key) && cfg[key].is_number_integer()) return cfg[key].get<int>();
        return fallback;
    };

    c.baseUrl = trim(str("baseUrl", c.baseUrl));
    while (!c.baseUrl.empty() && c.baseUrl.back() == '/') c.baseUrl.pop_back();
    if (c.baseUrl.empty()) c.baseUrl = "https://api.deepseek.com";

    c.apiKey = trim(str("apiKey", ""));
    c.apiKeyEnv = trim(str("apiKeyEnv", c.apiKeyEnv));
    c.timeoutMs = clampInt(num("timeoutMs", c.timeoutMs), 1000, 180000);
    c.defaultCount = clampInt(num("defaultCount", c.defaultCount), 1, 50);
    c.maxSummaryBytes = clampInt(num("maxSummaryBytes", c.maxSummaryBytes), 120, 4000);
    c.maxContentBytes = clampInt(num("maxContentBytes", c.maxContentBytes), 200, 20000);
    c.maxFetchBytes = clampInt(num("maxFetchBytes", c.maxFetchBytes), 512, 60000);
    c.maxResultBytes = clampInt(num("maxResultBytes", c.maxResultBytes), 800, 20000);
    return c;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
struct HttpOutcome {
    bool transportOk = false;   // 是否拿到 HTTP 响应（false = 网络层失败）
    long status = 0;
    std::string body;
    std::string error;          // transportOk=false 时的网络错误描述
};

HttpOutcome postJson(const SearchConfig& cfg, const std::string& path,
                     const nlohmann::json& payload) {
    HttpOutcome out;
    try {
        const cpr::Response r = cpr::Post(
            cpr::Url{cfg.baseUrl + path},
            cpr::Header{{"Authorization", "Bearer " + cfg.apiKey},
                        {"Content-Type", "application/json"},
                        {"Accept", "application/json"}},
            cpr::Body{payload.dump()},
            cpr::Timeout{std::chrono::milliseconds(cfg.timeoutMs)},
            cpr::ConnectTimeout{
                std::chrono::milliseconds(std::min(cfg.timeoutMs, 10000))});
        out.status = static_cast<long>(r.status_code);
        out.body = r.text;
        if (r.error.code != cpr::ErrorCode::OK) {
            out.error = r.error.message.empty() ? "传输层错误" : r.error.message;
            return out;
        }
        out.transportOk = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

// 把 HTTP 失败翻译成模型能看懂、且不泄密的错误文本
std::string describeHttpError(const std::string& what, const HttpOutcome& r,
                              const SearchConfig& cfg) {
    if (!r.transportOk) {
        return "error: " + what + " 请求失败（网络层）：" + r.error +
               "。请检查网络连通性或 baseUrl 配置。";
    }

    std::string detail;
    try {
        const auto j = nlohmann::json::parse(r.body);
        if (j.contains("error") && j["error"].is_object()) {
            detail = j["error"].value("message", "");
            const std::string type = j["error"].value("type", "");
            if (!type.empty()) detail += detail.empty() ? type : "（" + type + "）";
        }
    } catch (const std::exception&) {
        detail = cutUtf8(r.body, 200);
    }

    std::string hint;
    switch (r.status) {
    case 400: hint = "（请求参数不合法，改小 count 或调整 query 语法后重试）"; break;
    case 401: hint = "（鉴权失败：检查 deepseek-search 的 apiKey / " + cfg.apiKeyEnv + " 环境变量）"; break;
    case 402: hint = "（DeepSeek 账户余额不足，充值后重试）"; break;
    case 403: hint = "（该页面被安全策略拒绝抓取，换其他 URL）"; break;
    case 404: hint = "（未获取到可用的网页内容：URL 不可达、被站点拦截或无正文，换 URL 重试）"; break;
    case 408: hint = "（读取请求体超时，可稍后重试）"; break;
    case 413: hint = "（请求体超过大小限制）"; break;
    case 429: hint = "（触发 QPS 限速，稍等 1 秒后重试）"; break;
    default:
        if (r.status >= 500) hint = "（DeepSeek 服务端错误，稍后重试）";
        break;
    }

    std::string msg = "error: " + what + " HTTP " + std::to_string(r.status) + hint;
    if (!detail.empty()) msg += "：" + detail;
    return msg;
}

// ---------------------------------------------------------------------------
// 插件
// ---------------------------------------------------------------------------
class DeepSeekSearchPlugin final : public mio::sdk::Plugin {
public:
    DeepSeekSearchPlugin(mio::sdk::Host& host, const nlohmann::json& cfg)
        : host_(host), cfg_(parseConfig(cfg)) {}

    ~DeepSeekSearchPlugin() override { stop(); }

    bool start() override {
        if (cfg_.apiKey.empty()) {
            host_.error(kTag,
                        "未配置 DeepSeek API Key：请在 config.json 的 "
                        "plugins[deepseek-search].config.apiKey 填入，或设置环境变量 " +
                            cfg_.apiKeyEnv + "；插件不启动，web_search/web_fetch 不注册。");
            return false;
        }

        registerTools();
        host_.info(kTag, "已注册 web_search / web_fetch（baseUrl=" + cfg_.baseUrl +
                             ", count默认=" + std::to_string(cfg_.defaultCount) +
                             ", 超时=" + std::to_string(cfg_.timeoutMs) + "ms）");
        return true;
    }

    void stop() override {
        if (toolsRegistered_) {
            host_.unregisterTool("web_search");
            host_.unregisterTool("web_fetch");
            toolsRegistered_ = false;
        }
    }

private:
    void registerTools() {
        const bool okSearch = host_.registerTool(
            {{"name", "web_search"},
             {"description",
              "联网搜索网页，返回标题/链接/摘要/可选正文。由 DeepSeek 搜索服务提供，"
              "query 支持 site:、-site:、lang:、after:、before:、\"精确短语\"、-排除词 等搜索语法。"
              "需要实时信息、事实核查或找具体网页时使用。"},
             {"parameters",
              {{"type", "object"},
               {"properties",
                {{"query",
                  {{"type", "string"},
                   {"description", "搜索词，1~128 字符，可带搜索语法（如 DeepSeek site:github.com after:2025）"}}},
                 {"count",
                  {{"type", "integer"}, {"description", "返回条数 1~50，默认 5"}}},
                 {"need_content",
                  {{"type", "boolean"},
                   {"description", "是否在结果中返回网页正文（默认 false，仅摘要；正文会占用较多上下文）"}}},
                 {"ori_question",
                  {{"type", "string"},
                   {"description", "触发本次搜索的用户原始问题，有助于理解搜索意图（可选）"}}},
                 {"localization",
                  {{"type", "string"},
                   {"description", "用于本地化排序的 IP 地址（可选，一般不用填）"}}}}},
               {"required", nlohmann::json::array({"query"})}}}},
            [this](const nlohmann::json& args) { return doSearch(args); });

        const bool okFetch = host_.registerTool(
            {{"name", "web_fetch"},
             {"description",
              "抓取单个网页并返回 Markdown 正文，由 DeepSeek Fetch 服务提供。"
              "用于深读搜索结果、或用户直接给出 URL 时读取页面内容。"},
             {"parameters",
              {{"type", "object"},
               {"properties",
                {{"url",
                  {{"type", "string"},
                   {"description", "要抓取的网页地址，必须是含 host 的 http/https URL"}}}}},
               {"required", nlohmann::json::array({"url"})}}}},
            [this](const nlohmann::json& args) { return doFetch(args); });

        toolsRegistered_ = okSearch || okFetch;
        if (!okSearch) host_.warn(kTag, "注册工具 web_search 失败（重名或注册表拒绝）");
        if (!okFetch) host_.warn(kTag, "注册工具 web_fetch 失败（重名或注册表拒绝）");
    }

    // ---- web_search ----
    std::string doSearch(const nlohmann::json& args) {
        const std::string query = trim(jsonStr(args, "query"));
        if (query.empty()) return "error: query 不能为空";

        int count = clampInt(jsonInt(args, "count", cfg_.defaultCount), 1, 50);

        nlohmann::json payload = {{"query", query}, {"count", count}};
        if (args.contains("need_content") && args["need_content"].is_boolean()) {
            payload["need_content"] = jsonBool(args, "need_content", false);
        }
        if (args.contains("ori_question") && args["ori_question"].is_string()) {
            const std::string q = trim(args["ori_question"].get<std::string>());
            if (!q.empty()) payload["ori_question"] = q;
        }
        if (args.contains("localization") && args["localization"].is_string()) {
            const std::string ip = trim(args["localization"].get<std::string>());
            if (!ip.empty()) payload["localization"] = ip;
        }

        const bool needContent =
            payload.contains("need_content") && payload["need_content"].get<bool>();
        host_.debug(kTag, "web_search query=" + query + " count=" + std::to_string(count) +
                              (needContent ? " need_content=true" : ""));

        const HttpOutcome r = postJson(cfg_, kSearchPath, payload);
        if (!r.transportOk || r.status != 200) {
            host_.warn(kTag, "web_search 失败 status=" + std::to_string(r.status));
            return describeHttpError("DeepSeek Search", r, cfg_);
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(r.body);
        } catch (const std::exception& e) {
            return std::string("error: DeepSeek Search 返回了非法 JSON：") + e.what();
        }
        if (!body.contains("results") || !body["results"].is_array()) {
            return "error: DeepSeek Search 返回格式异常（缺少 results 数组）：" +
                   cutUtf8(r.body, 300);
        }

        const auto& results = body["results"];
        host_.info(kTag, "web_search query=" + query + " 命中 " +
                             std::to_string(results.size()) + " 条");

        std::string out = "DeepSeek Search（query: " + query + "）返回 " +
                          std::to_string(results.size()) + " 条结果：\n";
        std::size_t shown = 0;
        for (const auto& item : results) {
            if (!item.is_object()) continue;

            std::string title = trim(jsonStr(item, "title"));
            if (title.empty()) title = "(无标题)";
            std::string entry;
            entry += std::to_string(shown + 1) + ". " + title + "\n";
            const std::string url = trim(jsonStr(item, "url"));
            if (!url.empty()) entry += "   URL: " + url + "\n";

            std::string meta;
            auto appendMeta = [&meta](const std::string& label, const nlohmann::json& v) {
                if (v.is_string()) {
                    const std::string s = trim(v.get<std::string>());
                    if (s.empty()) return;
                    if (!meta.empty()) meta += " | ";
                    meta += label + s;
                } else if (v.is_number()) {
                    if (!meta.empty()) meta += " | ";
                    meta += label + v.dump();
                }
            };
            appendMeta("来源: ", item.value("site_name", nlohmann::json(nullptr)));
            appendMeta("发布: ", item.value("date", nlohmann::json(nullptr)));
            appendMeta("权威性: ", item.value("authority", nlohmann::json(nullptr)));
            appendMeta("相关度: ", item.value("rerank_score", nlohmann::json(nullptr)));
            if (!meta.empty()) entry += "   " + meta + "\n";

            std::string summary = jsonStr(item, "summary");
            if (summary.empty()) summary = jsonStr(item, "snippet");
            summary = trim(summary);
            if (!summary.empty()) {
                const bool cut = summary.size() > static_cast<std::size_t>(cfg_.maxSummaryBytes);
                entry += "   摘要: " + cutUtf8(summary, cfg_.maxSummaryBytes);
                entry += cut ? "…\n" : "\n";
            }

            if (needContent) {
                const std::string content = trim(jsonStr(item, "content"));
                if (!content.empty()) {
                    const bool cut = content.size() > static_cast<std::size_t>(cfg_.maxContentBytes);
                    entry += "   正文: " + cutUtf8(content, cfg_.maxContentBytes);
                    entry += cut ? "…\n" : "\n";
                }
            }

            // 输出预算：超了就停（至少保留 1 条，避免模型什么都看不到）
            if (shown > 0 && out.size() + entry.size() > static_cast<std::size_t>(cfg_.maxResultBytes)) {
                out += "…已省略 " + std::to_string(results.size() - shown) +
                       " 条（超出单次工具输出预算）：可减小 count、用 site:/lang:/after: 收窄 query，"
                       "或对感兴趣的 URL 调 web_fetch 深读。\n";
                break;
            }
            out += entry;
            ++shown;
        }

        if (shown == 0) out += "（没有可用结果）\n";
        return out;
    }

    // ---- web_fetch ----
    std::string doFetch(const nlohmann::json& args) {
        const std::string url = trim(jsonStr(args, "url"));
        if (url.empty()) return "error: url 不能为空";
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            return "error: url 必须是 http/https 地址（含 host）";

        host_.debug(kTag, "web_fetch url=" + url);

        const HttpOutcome r = postJson(cfg_, kFetchPath, {{"url", url}});
        if (!r.transportOk || r.status != 200) {
            host_.warn(kTag, "web_fetch 失败 url=" + url +
                                 " status=" + std::to_string(r.status));
            return describeHttpError("DeepSeek Fetch", r, cfg_);
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(r.body);
        } catch (const std::exception& e) {
            return std::string("error: DeepSeek Fetch 返回了非法 JSON：") + e.what();
        }

        const std::string title = trim(jsonStr(body, "title"));
        const std::string content = trim(jsonStr(body, "content"));
        if (content.empty()) return "error: DeepSeek Fetch 未返回可用的网页正文：" + url;

        host_.info(kTag, "web_fetch ok url=" + url + " 正文 " +
                             std::to_string(content.size()) + " 字节");

        std::string out;
        out += "DeepSeek Fetch: " + (title.empty() ? "(无标题)" : title) + "\n";
        out += "URL: " + url + "\n";
        out += "---\n";
        out += cutUtf8(content, cfg_.maxFetchBytes);
        if (content.size() > static_cast<std::size_t>(cfg_.maxFetchBytes)) {
            out += "\n…[正文已截断：共 " + std::to_string(content.size()) + " 字节，"
                   "仅返回前 " + std::to_string(cfg_.maxFetchBytes) + " 字节]";
        }
        return out;
    }

    mio::sdk::Host& host_;
    SearchConfig cfg_;
    bool toolsRegistered_ = false;
};

} // namespace

MIO_PLUGIN(DeepSeekSearchPlugin, "deepseek-search", "0.1.0")
