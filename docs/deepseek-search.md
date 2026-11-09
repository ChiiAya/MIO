# DeepSeek 网页搜索 / 抓取插件（`deepseek-search`）

MIO 内置的原生插件 `deepseek-search` 通过 DeepSeek 官方的 Search / Fetch 工具 API 给模型提供两个联网工具：

| 工具 | 作用 | 对应接口 |
|---|---|---|
| `web_search` | 按 query 搜索网页，返回标题 / 链接 / 摘要 / 可选正文 | `POST /v1/tools/search` |
| `web_fetch` | 抓取单个网页，返回 Markdown 正文 | `POST /v1/tools/fetch` |

- 官方文档：[Search API](https://api-docs.deepseek.com/zh-cn/api/search.md) / [Fetch API](https://api-docs.deepseek.com/zh-cn/api/fetch.md)
- 实现位置：[plugins/websearch/src/DeepSeekSearchPlugin.cpp](../plugins/websearch/src/DeepSeekSearchPlugin.cpp)

> **为什么是原生插件而不是 Lua？**
> Lua 沙箱没有网络能力（无 `io`/`os`/`package`/socket 绑定，也没有 `mio.http`），
> 详见 [插件开发指南 §2.8](plugin-guide.md#28-已知限制务必阅读)。联网类功能必须走
> Native 插件或 MCP。Lua 脚本仍然可以**调用**本插件注册的工具（见下文「Lua 桥接」）。

---

## 1. 启用

在 `config.json` 的 `plugins` 数组里加入（`config.example.json` 已带一份注释版模板，默认关闭）：

```json
{
  "plugins": [
    { "id": "console", "enabled": true, "config": {} },
    {
      "id": "deepseek-search",
      "enabled": true,
      "config": {
        "apiKey": "sk-你的-deepseek-key",
        "baseUrl": "https://api.deepseek.com",
        "timeoutMs": 30000,
        "defaultCount": 5
      }
    }
  ]
}
```

- `apiKey` 留空时读取环境变量（默认 `DEEPSEEK_API_KEY`，可用 `apiKeyEnv` 改名）：
  ```bash
  export DEEPSEEK_API_KEY=sk-你的-deepseek-key
  ```
- **Key 必须来自 DeepSeek 平台**。`openai.apiKey` 指向小米 MiMo / 其他兼容端点时不能复用；
  Search/Fetch 的鉴权与 DeepSeek 主 API 相同（`Authorization: Bearer <key>`）。
- 插件在 `start()` 时校验 Key：缺失会打日志并**拒绝启动**，两个工具不会注册给模型
  （比注册一个永远报错的工具更省 token 和回合）。

### 配置项

| 字段 | 默认值 | 说明 |
|---|---|---|
| `apiKey` | `""` | DeepSeek API Key；留空回退到环境变量 |
| `apiKeyEnv` | `"DEEPSEEK_API_KEY"` | `apiKey` 为空时读取的环境变量名 |
| `baseUrl` | `"https://api.deepseek.com"` | 可覆盖为本地 mock / 代理地址（测试用） |
| `timeoutMs` | `30000` | 单次 HTTP 超时（1000~180000） |
| `defaultCount` | `5` | `web_search` 未传 `count` 时的默认条数（1~50） |
| `maxSummaryBytes` | `720` | 每条 `summary` 最多保留字节数（UTF-8 安全截断） |
| `maxContentBytes` | `2400` | `need_content=true` 时每条正文最多字节数 |
| `maxFetchBytes` | `3800` | `web_fetch` 正文最多字节数 |
| `maxResultBytes` | `3800` | `web_search` 单次输出总预算，超出后截断并提示「可减小 count / 收窄 query」 |

> 输出预算默认压在 4KB 以内，是因为 ToolLoop 的**超大结果截断护栏**默认阈值为
> 4096 字节（超出会落盘到 `data/tool_overflow/` 只回传预览）。把搜索结果保持在
> 4KB 内，模型才能完整读到摘要与正文，而不是只拿到一个文件路径。

---

## 2. 工具说明

### `web_search`

```json
{
  "query": "DeepSeek V4 发布 site:example.com",
  "count": 5,
  "need_content": false,
  "ori_question": "用户原话（可选，帮助理解搜索意图）",
  "localization": "IP 地址（可选，本地化排序）"
}
```

- `query` 支持 DeepSeek 搜索语法：`site:` / `-site:` / `lang:` / `after:` / `before:` /
  `"精确短语"` / `-排除词`。
- 返回文本形如：

  ```text
  DeepSeek Search（query: DeepSeek V4 发布 site:example.com）返回 3 条结果：
  1. DeepSeek V4 模型发布
     URL: https://example.com/deepseek-v4
     来源: Example News | 发布: 2026-06-01T00:00:00+00:00 | 权威性: 3 | 相关度: 2.4
     摘要: DeepSeek 发布了 V4 模型……
     正文: ……（need_content=true 时才有）
  …
  已省略 2 条（超出单次工具输出预算）…
  ```

- 默认只返回摘要；`need_content=true` 才带正文（正文会显著吃上下文，按需开启）。

### `web_fetch`

```json
{ "url": "https://example.com/article" }
```

- URL 必须是含 host 的 `http://` / `https://` 地址。
- 返回 `标题 / URL / 正文`，正文超出 `maxFetchBytes` 会截断并标注原始字节数。

### 错误语义

插件把 HTTP 错误翻译成模型可读的文本（不会抛异常打断回合），例如：

- `401` → `error: DeepSeek Search HTTP 401（鉴权失败：检查 deepseek-search 的 apiKey / DEEPSEEK_API_KEY 环境变量）：invalid api key`
- `404`（Fetch）→ 提示 URL 不可达 / 被站点拦截 / 无正文
- `429` → 提示触发 QPS 限速，稍后重试
- 网络层失败（DNS/超时）→ `error: … 请求失败（网络层）：…`

---

## 3. 计费与限速

- 按**成功请求次数**计费：Search / Fetch 各 **5 元 / 1000 次**（≈0.005 元/次）。
- 失败、超时、Search 空结果、Fetch 无正文都不计费。
- `count` 只影响返回条数，不影响单次价格——所以**优先用较小 `count` + 精确 query**，
  不要靠一次拉 50 条来"省钱"。
- 账号级限速：Search 100 QPS、Fetch 25 QPS（Fetch 另有按目标域名的限速）。
- 该 API 独立于主对话模型计费，模型每次主动调 `web_search` 都会产生一次请求费用。

---

## 4. Lua 桥接（可选）

Lua 脚本无法自己联网，但可以通过 `mio.invoke_tool` **同步调用**本插件的工具
（原生插件工具，不会触发 Lua 沙箱互锁）：

```lua
mio.register_tool({
  name = "lua_web_search",
  description = "Lua 桥接：调用 DeepSeek 搜索",
  parameters = {
    type = "object",
    properties = { query = { type = "string", description = "搜索词" } },
    required = { "query" },
  },
}, function(args)
  return mio.invoke_tool("web_search", { query = args.query, count = 3 })
end)
```

也可以只在 `mio.every` / `on_event` 回调里调用 `web_search`，把结果写进记忆或再
`mio.post_event` 投递。注意：`mio.invoke_tool` 是同步阻塞的，搜索请求期间该 Lua
沙箱的定时器/Hook 会等待，不要在高频回调里做这件事。

---

## 5. 排障

| 现象 | 原因 / 处理 |
|---|---|
| 日志出现 `未配置 DeepSeek API Key` / `启动插件失败: deepseek-search` | 填 `apiKey` 或导出 `DEEPSEEK_API_KEY` 后重启；`/plugin list` 里该插件应为 stopped |
| 模型完全看不到 `web_search` / `web_fetch` | 插件被 `enabled: false`、启动失败，或 `plugins` 数组没列出该 id（数组存在时只加载列出的插件） |
| 返回 `HTTP 401` | Key 无效 / 不是 DeepSeek 平台 Key |
| 返回 `HTTP 402` | DeepSeek 账户余额不足 |
| 返回 `HTTP 429` | 触发 QPS 限速，稍等重试；高频调用请加缓存或降频 |
| 返回 `HTTP 404`（fetch） | 站点反爬 / URL 失效 / 内容无正文，换 URL |
| 想本地联调不花钱 | 把 `baseUrl` 指向本地 mock（见 `build/itest/mock_ds_api.py`，gitignored 测试资产） |

本地回归脚本（gitignored 测试资产，需先 `xmake` 构建 release）：

```bash
bash build/itest/run_websearch_test.sh
```

覆盖：工具注册、请求体/鉴权、`need_content` 正文、UTF-8 截断、超预算省略、401 错误回喂、
Lua 桥接调用、缺 Key 时拒绝启动。
