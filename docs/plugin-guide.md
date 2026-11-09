# MIO 插件开发指南（Lua / Native）

> 适用版本：`MIO_PLUGIN_ABI_VERSION = 1`（`sdk/include/mio/plugin_abi.h`）
> 本文同时覆盖两条插件路线：
> - **Lua 轻量插件**：把 `.lua` 文件丢进脚本目录即可，无需编译，支持热重载。
> - **Native（C++）插件**：编译进主程序，拥有完整能力（注册出站通道、Hook、多线程、文件/网络、系统集成）。

---

## 0. 先看结论：选哪条路线？

| 能力 | Lua 插件 | Native 插件 |
|---|---|---|
| 注册 LLM 工具 | ✅ `mio.register_tool` | ✅ `host.registerTool` |
| 入站事件 Hook（过滤/改写） | ✅ `mio.on_event` | ✅ `register_hook("on_event")` |
| 出站消息 Hook（过滤/改写） | ✅ `mio.on_outbound` | ✅ `register_hook("on_outbound")` |
| 请求前 Hook（追加 system hint） | ❌ | ✅ `register_hook("on_before_llm")` |
| 注册出站平台通道（实现一个平台适配器） | ❌ | ✅ `host.registerChannel` |
| 定时任务 | ✅ `mio.every` + `mio.local_time()` | ✅ 自建线程（必须自行 join） |
| 主动投递入站事件 | ✅ `mio.post_event`（异步队列投递） | ✅ `host.postEvent` |
| 读取 `<dataDir>/plugins/<id>/` 数据目录 | ❌ | ✅ `host.dataDir()` |
| 触发配置热重载 / 查询状态 / 请求退出 | ❌ | ✅ `host.reloadConfig/stateJson/requestShutdown` |
| 第三方 C++ 依赖（网络、ASR、数据库…） | ❌ | ✅ 在 `xmake.lua` 加包 |
| 调用外部 HTTP API（搜索/抓取/第三方服务） | ❌（只能 `mio.invoke_tool` 桥接 Native/MCP 工具） | ✅ 直接用 cpr 发请求（见 `deepseek-search`） |
| 部署方式 | 放文件 + `/plugin reload` | 重新编译并重启 MIO |
| 跨热重载保留内存状态 | ❌ 沙箱销毁重建 | ✅ `reconfigure()` 可保留 |

一句话：**给模型加工具、做消息过滤/改写 → Lua；要实现一个新平台、接入硬件/网络/外部服务 → Native。**

---

## 1. 插件运行模型（两条路线共用）

### 1.1 插件在主链路中的位置

```
入站事件（平台插件 / Lua / MCP）
   │  host.post_event(Event JSON)
   ▼
Runtime::postEvent ──► on_event Hook 链 ──► RelationshipGraph.onSeen ──► AttentionGate
   │                                                                    │（归档 Achieve）
   │                                                                    ▼
   │                                                              MandLoop 拉取批次
   │                                                                    │
   │              on_before_llm Hook 链 ◄───────────────────────────────┤
   │                                                                    ▼
   │                                          ContextBuilder → ToolLoop（工具由 ToolRegistry 提供）
   │                                                                    │
   │                                                                  emit()
   ▼                                                                    ▼
PluginManager ──► on_outbound Hook 链 ──► OutboundRouter（[SEP] 拆气泡 + 打字延迟）
                                                                        │
                                                                        ▼
                                                      注册过的平台通道 channel.send()
```

插件可以参与的位置：**工具**、**三类 Hook**、**出站通道**、**入站事件生产者**、**独立定时/后台任务**。

### 1.2 生命周期

宿主启动与关闭顺序（`src/main.cpp`）：

```
PluginManager::loadAll()   → 对每个已启用插件：create(host_api, config_json)
PluginManager::startAll()  → start()             （必须非阻塞，自己起线程）
    ...运行（工具调用 / Hook / 通道发送 / 定时器）...
PluginManager::stopAll()   → stop() → destroy()
```

- `create()`：构造对象、注册工具/通道/Hook、解析配置。**不要在这里做阻塞 IO**。
- `start()`：启动后台线程或网络服务，立即返回。
- `stop()`：停止并 **join 所有自有线程**；可被重复调用（要幂等）。
- `destroy()`：释放对象。之后不得再使用 host 指针。
- `reconfigure(new_config)`：配置热重载时优先调用；返回 `0` 表示已生效，返回非 0 表示宿主退化为 `stop → destroy → create → start` 重启该插件。

### 1.3 线程模型（很重要）

| 回调 | 调用线程 | 约束 |
|---|---|---|
| `create / start / stop / destroy / reconfigure` | 主线程（或管理请求线程） | 不能长期阻塞 |
| `post_event`（你主动投递） | 任意线程（宿主内部加锁 / Lua 侧异步入队） | 可安全调用 |
| `on_event` Hook | 投递事件的那个线程 | Lua 侧在回调里 `mio.post_event` 是安全的（异步入队）；Native 侧不要在 hook 里再 `postEvent`（会递归） |
| `on_before_llm` Hook | MindLoop Worker | 极轻量 |
| `on_outbound` Hook / `channel.send` | 触发外发的线程（通常是 MindLoop Worker） | **不要阻塞/睡长觉**，会拖慢整个意识流回合 |
| 工具 `fn` | MindLoop Worker（同步） | 长耗时工具会阻塞回合；中断只能在工具返回后生效 |
| `mio.every` 回调（Lua） | Lua 插件自己的定时线程 | 见 §2.8 的限制 |

### 1.4 owner / source / channelId 三个名字

- **owner**：宿主登记归属用的前缀。Native 插件 = `plugin:<id>`；Lua 工具 = `lua:<脚本名>`。卸载插件时按 owner 批量清理工具/通道/Hook。
- **source**：平台/适配器标识，例如 `console`、`napcat`、`lua:dice`、`plugin:echo`。出站消息按 `source` 找通道。
- **channelId**：会话标识，约定 `private:<uid>`、`group:<gid>` 这类「类型:主体」格式（由各平台自定）。

---

## 2. Lua 插件开发

### 2.1 目录与脚本结构

Lua 插件由内置的 `lua-host` 原生插件托管（`plugins/lua/src/LuaHostPlugin.cpp`）。

```
plugins/lua/            ← 默认脚本目录（config: plugins[] 中 lua-host 的 dir 字段）
├── dice.lua            ← 一个脚本 = 一个插件，文件名（不含 .lua）= 脚本 ID
└── ...
```

`lua-host` 启动时按文件名排序加载目录下所有 `*.lua`，每个脚本一个独立 `lua_State` 沙箱。
脚本文件返回一个可选元数据表：

```lua
return {
    name = "dice",       -- 展示名（可省略）
    version = "0.1.0"    -- 版本（可省略，仅用于日志）
}
```

配置方式（`config.json`）：

```json
{
  "plugins": [
    { "id": "lua-host", "enabled": true, "config": { "dir": "plugins/lua" } }
  ]
}
```

> `lua-host` 在 `MIO_PLATFORM` 环境变量存在时会自动启用；没有 `plugins` 配置段时按 `platform` 字段启用。

### 2.2 `mio` API 一览

| API | 说明 |
|---|---|
| `mio.log.debug/info/warn/error(msg)` | 打日志，tag 自动为 `lua:<脚本名>` |
| `mio.register_tool(def, fn)` | 注册 LLM 工具，返回 bool |
| `mio.on_event(fn)` / `mio.on_outbound(fn)` | 注册 Hook，可注册多个，按注册顺序执行 |
| `mio.every(seconds, fn)` | 定时器，最小 1 秒 |
| `mio.local_time()` | 本地时间字符串 `"YYYY-MM-DD HH:MM:SS"`（按进程时区） |
| `mio.now_ms()` | 当前 Unix 时间戳（毫秒） |
| `mio.invoke_tool(name, args_table)` | 同步调用其他工具（内置/MCP/Native），返回字符串；失败返回 `error: ...` |
| `mio.refresh_facts()` | 请求宿主立即重建稳定 System Prompt（消费通讯录/Facts 脏标记） |
| `mio.json.encode(v)` / `mio.json.decode(s)` | JSON 编解码 |
| `mio.post_event(event_table)` | 异步投递入站事件；返回 `true` 表示已入队。跨会话自省/定时唤醒靠它，见 §2.5 |

Lua 沙箱里**没有** `io`、`os`、`require`、`debug`、`coroutine`，也没有 `dofile` / `loadfile`；可用库只有 base、string、table、math、utf8。

### 2.3 工具开发

工具定义 JSON 与 Native 一致：`name` / `description` / `parameters`（JSON Schema）/ `terminal`（可选）。
`owner` 会被宿主强制改写成 `lua:<脚本名>`，不需要自己填。

最小可用示例（仓库内 [plugins/lua/dice.lua](../plugins/lua/dice.lua)）：

```lua
mio.register_tool({
    name = "roll_dice",
    description = "掷骰子（支持 NdM 格式，如 2d6、1d20、3d100）。",
    parameters = {
        type = "object",
        properties = {
            expr = { type = "string", description = "骰子表达式，如 '2d6' 或 '1d20'" }
        },
        required = { "expr" }
    }
}, function(args)
    local expr = tostring(args.expr or "1d6")
    local n_str, m_str = expr:match("^(%d+)[dD](%d+)$")
    local n = math.max(1, math.min(tonumber(n_str) or 1, 100))
    local m = math.max(2, math.min(tonumber(m_str) or 6, 10000))

    local rolls, sum = {}, 0
    for i = 1, n do
        local r = math.random(1, m)
        rolls[i] = tostring(r)
        sum = sum + r
    end
    return string.format("%dd%d = [%s] (合计: %d)", n, m, table.concat(rolls, ", "), sum)
end)
```

返回值约定：

- 返回 **string** → 直接作为 `role=tool` 内容回喂给模型；
- 返回 **table / number / boolean** → 自动 JSON 序列化后回喂；
- 返回 **nil** → 空字符串；
- 抛出 Lua 运行时错误 → 工具循环收到 `error: Lua tool [name] failed: ...`，模型可据此自纠，宿主不会崩。

参数会被 ToolLoop 按 schema 白名单过滤（未声明的键直接丢弃），因此 **schema 里没写的参数勿依赖**。同类工具建议只注册一个名字，避免模型在多个同义工具间摇摆。

### 2.4 Hook：过滤、改写、拦截

Lua 侧只暴露两个 Hook 点，它们挂在宿主 `on_event` / `on_outbound` 链上（priority = 100）。

**`mio.on_event(function(event) ... end)`**

- 返回 `nil`（或不返回）→ 放行；
- 返回 `false` → **丢弃**该事件（不会进归档、不会唤醒模型）；
- 返回一个 table → 用该 table **替换**事件 JSON 后继续放行（后续 Hook 看到的是替换后的内容）。

```lua
mio.on_event(function(ev)
    -- 丢弃来自测试群的消息
    if ev.channelId == "group:123456" then
        return false
    end
    -- 给所有 console 消息加标记
    if ev.source == "console" then
        ev.title = (ev.title or "") .. "#debug"
        return ev
    end
    return nil
end)
```

**`mio.on_outbound(function(outbound) ... end)`**

载荷字段：`source`、`channelId`、`text`、`replyTo`、`mentions`（注意：**不包含** `parts` 多模态附件，Lua 也无法改写附件）。

- 返回 `nil` → 放行；返回 `false` → 丢弃这条出站消息（宿主记日志，不报错）；
- 返回 table → 用其中的 `source` / `channelId` / `text` / `replyTo` 覆盖原值。

```lua
mio.on_outbound(function(out)
    out.text = out.text:gsub("…", "...")
    if #out.text > 4500 then
        out.text = out.text:sub(1, 4500)   -- 简单长度兜底
    end
    return out
end)
```

> Hook 是**同步**执行在消息主链路上的：里面不要 `os.execute`、不要死循环、不要做秒级耗时操作。Lua 沙箱本身也没有 `os`。

### 2.5 定时器

```lua
-- 每 60 秒检查一次外部状态（示例：调用内置工具读时间）
mio.every(60, function()
    local now = mio.invoke_tool("get_current_time", {})
    mio.log.info("tick: " .. now)
end)
```

- 单位秒，最小 1 秒，实际调度精度受定时线程的等待策略影响；
- 回调在**定时线程**执行，且沙箱会持有内部 VM 锁 → 不要在回调里做长阻塞；
- 同一脚本内多个 `mio.every` 共用一个定时线程，按到期顺序串行执行；
- 需要按墙钟时间触发时用 `mio.local_time()` / `mio.now_ms()`（沙箱没有 `os` 库）。

按天定时唤醒模型的完整示例见 [plugins/lua/dreaming.lua](../plugins/lua/dreaming.lua)：每天 03:00-03:59 窗口内首次检查时 `mio.post_event` 一条 `InternalPulse` 事件，让模型回顾当天并写入长期记忆，随后 `end_turn` 静默结束回合。

### 2.6 沙箱限制与错误语义

| 限制 | 值 | 表现 |
|---|---|---|
| 单脚本内存 | 8 MB | 超限触发 Lua OOM，脚本调用/定时器报错，不拖垮宿主 |
| 单次调用指令数 | 500,000 条 | 死循环会被强制中断并报 `exceeded instruction quota` |
| 可用标准库 | base/string/table/math/utf8 | 无 IO、无网络、无 `require` |
| 脚本执行错误 | 捕获并 `warn` | 工具调用返回 `error: ...`；Hook/定时器只记日志 |

因此：**Lua 插件是纯逻辑、无持久化侧效的**。需要落盘、联网、读环境变量，走 Native。

### 2.7 热重载

```text
/plugin reload lua:dice        # 只重载 dice.lua（推荐）
/plugin reload                 # 等于 /plugin reload lua-host：重载整个脚本目录
/plugin reload all             # 重启全部插件
```

HTTP 等价接口：

```bash
curl -X POST http://127.0.0.1:6188/api/plugins/reload -d '{"id":"lua:dice"}'
```

语义（`PluginManager::reloadPlugin` → `LuaHostPlugin::reconfigure`）：

1. 先同步 `config.json` 中该插件的最新 `config`；
2. 找到 `<dir>/<id>.lua`，销毁旧沙箱（注销其全部工具，等待在途工具调用归零）并创建新沙箱；
3. 脚本重新执行一遍：重新 `register_tool`、重新注册 Hook、重新登记定时器。

注意：

- 脚本 ID 就是文件名 stem，**重命名脚本要及时清理旧工具**（旧沙箱销毁时会按 owner 注销，一般无需手动处理）；
- 不同脚本注册了同名工具时，后注册者覆盖前者（`ToolRegistry` 是 map）；
- 热重载会丢失 Lua 全局状态；需要持久状态就写 Native 插件或落外部存储。

### 2.8 已知限制（务必阅读）

以下限制来自当前实现（`LuaHostPlugin.cpp`），不是配置问题：

1. **`mio.post_event` 是异步投递**：事件先进入插件自己的队列，由独立线程在沙箱锁之外调用 `host.post_event`。因此返回 `true` 只表示「已入队」，不是「已送达」；插件停止时会丢弃尚未投递的事件。不要在回调里无限自投递（队列上限 256，满了会 warn 并丢弃）。
2. **不要在 Lua 回调里 `mio.invoke_tool` 调用「注册在 Lua 里的工具」**（包括自己）：会再次进入同一个/别的 Lua 沙箱取锁 → 死锁。调用内置工具（`get_current_time` 等）或 MCP/原生工具是安全的。
3. Lua 插件不能注册出站通道，也不能读取 `dataDir`、热重载配置、查询状态或请求退出——这些只有 Native 侧有对应 host API。
4. `mio.every` 的回调里 `mio.refresh_facts()` 是安全的；但它会重建 System Prompt，进而触发宿主的历史压缩（前缀缓存失效保护），属于预期行为。

---

## 3. Native（C++）插件开发

### 3.1 两种头文件，选一种

```
sdk/include/mio/plugin_abi.h   ← 纯 C ABI：结构体、函数指针、生命周期描述符、ABI 版本
sdk/include/mio/plugin.hpp     ← Header-only C++ SDK：Host 封装、Plugin/Channel 基类、MIO_PLUGIN 宏
```

> **当前构建只支持静态插件**：`xmake.lua` 给 `MIO` target 定义了 `MIO_PLUGIN_STATIC`，`PluginManager::loadAll()` 只遍历 `sdk::staticPluginRegistry()`。
> `mio_plugin_entry` 动态导出分支（不定义 `MIO_PLUGIN_STATIC` 时）是 ABI 预留，宿主尚未实现 `dlopen` 加载器——所以 Native 插件要**编译进主程序**。

### 3.2 最小可运行示例

目录：`plugins/echo/src/EchoPlugin.cpp`（目录名随意，`plugins/**/src/**.cpp` 会被自动编译）。

```cpp
#include "mio/plugin.hpp"

#include <atomic>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

class EchoPlugin final : public mio::sdk::Plugin, public mio::sdk::Channel {
public:
    EchoPlugin(mio::sdk::Host& host, const nlohmann::json& cfg)
        : host_(host), prefix_(cfg.value("prefix", "[echo] ")) {
        // 1) 注册出站通道：source = "echo"
        host_.registerChannel("echo", this);

        // 2) 注册一个 LLM 工具
        host_.registerTool(
            {{"name", "echo"},
             {"description", "把一段文本原样回显，用于调试。"},
             {"parameters",
              {{"type", "object"},
               {{"properties", {{"text", {{"type", "string"}}}}}},
               {"required", nlohmann::json::array({"text"})}}}},
            [this](const nlohmann::json& args) -> std::string {
                return prefix_ + args.value("text", "");
            });

        // 3) 注册入站 Hook：丢掉含 "spam" 的事件
        host_.registerHook("on_event", /*priority=*/50,
                           [](const nlohmann::json& ev, bool& drop)
                               -> std::optional<nlohmann::json> {
                               if (ev.value("content", "").find("spam") != std::string::npos) {
                                   drop = true;
                               }
                               return std::nullopt;
                           });
    }

    ~EchoPlugin() override { stop(); }

    bool start() override {
        if (running_.exchange(true)) return true;
        worker_ = std::thread([this] {
            while (running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                // 真实插件在这里处理网络/串口/文件事件
            }
        });
        return true;
    }

    void stop() override {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }

    // sdk::Channel：接收 MIO 的出站消息
    bool send(const nlohmann::json& outbound) override {
        std::cout << prefix_ << outbound.value("channelId", "") << " | "
                  << outbound.value("text", "") << "\n";
        return true;
    }

    nlohmann::json capabilities() const override {
        return {{"reply", true}, {"mention", false}, {"maxLen", 4500}};
    }

private:
    mio::sdk::Host& host_;
    std::string prefix_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace

MIO_PLUGIN(EchoPlugin, "echo", "0.1.0")
```

启用方式（`config.json`）：

```json
{
  "plugins": [
    { "id": "console", "enabled": true, "config": {} },
    { "id": "echo",    "enabled": true, "config": { "prefix": "[echo] " } }
  ]
}
```

> `plugins` 数组存在时**只加载数组里列出的插件**（按 `id` 精确匹配）；只有数组缺省/为空时才回退到 `platform` 字段 + `MIO_PLATFORM` 环境变量。

### 3.3 MIO_PLUGIN 宏做了什么

```cpp
MIO_PLUGIN(PluginClass, "id", "version")
```

- 生成 `mio_plugin_desc`（`struct_size` / `abi_version` / `id` / `version` / `create` / `start` / `stop` / `destroy` / `reconfigure`）；
- 静态模式（`MIO_PLUGIN_STATIC`）下注册到全局静态插件表；
- 动态模式（不定义该宏）下额外导出 `extern "C" const mio_plugin_desc* mio_plugin_entry()`；
- `PluginClass` 构造签名必须是 `(mio::sdk::Host&, const nlohmann::json&)`；
- 插件类继承 `mio::sdk::Plugin`：`start()` / `stop()` 纯虚，`reconfigure(cfg)` 返回 bool（默认 false = 需要重启插件）。

### 3.4 目录与构建

```
plugins/<id>/
├── include/...        ← 私有头文件（可选；需在 xmake.lua 加 add_includedirs）
└── src/*.cpp          ← 自动被 add_files("plugins/**/src/**.cpp") 收录
```

第三方依赖（cpr / nlohmann_json / ixwebsocket / lua5.4 已内置）不够用时，在根 `xmake.lua`：

```lua
add_requires("yours_lib")
target("MIO")
    add_packages("yours_lib")
```

改完执行 `xmake f -m release && xmake`。插件不是独立 target，**必须重新构建主程序**。

### 3.5 Host API 参考

C ABI 定义在 `sdk/include/mio/plugin_abi.h`；C++ 封装在 `mio::sdk::Host`。

| C 函数 | SDK 方法 | 说明 |
|---|---|---|
| `log` | `host.log/debug/info/warn/error` | 日志，tag 自定义 |
| `post_event` | `host.postEvent(json)` | 投递入站事件（`Event` JSON，见 §3.8） |
| `register_tool` | `host.registerTool(def, fn)` | 注册工具，owner 自动为 `plugin:<id>` |
| `unregister_tool` | `host.unregisterTool(name)` | 注销单个工具 |
| `register_channel` | `host.registerChannel(source, channel*)` | 注册出站通道（一个 source 一个通道） |
| `invoke_tool` | `host.invokeTool(name, args)` | 同步调用其他工具（含 MCP） |
| `data_dir` | `host.dataDir()` | 返回并自动创建 `<dataDir>/plugins/<id>/` |
| `reload_config` | `host.reloadConfig(path = "")` | 触发热重载（空 = 当前配置文件） |
| `state_json` | `host.stateJson()` | 轻量运行状态（messageCount/botName/…） |
| `request_shutdown` | `host.requestShutdown()` | 让主进程优雅退出 |
| `refresh_facts` | `host.refreshFacts()` | 立即重建稳定 System Prompt（消费脏标记） |
| `register_hook` | `host.registerHook(point, priority, fn)` | Hook 注册（三个点见 §3.7） |
| `plugins_json` | `host.pluginsJson()` | 已加载插件列表与各自工具 |
| `reload_plugin` | `host.reloadPlugin(id = "")` | 热重载插件（空 = lua-host） |

`sdk::Host` 还提供 `raw()` 拿到底层 `mio_host_api*`。**跨 ABI 回调不能把 C++ 异常抛过去**：SDK 封装已 `try/catch` 兜底；如果直接用 C ABI 手写，必须自己保证 noexcept。

### 3.6 工具、通道、Hook 的契约

**工具定义 `def` JSON**

```json
{
  "name": "get_weather",
  "description": "查询城市天气（模型据此决定何时调用，写清楚输入输出与限制）",
  "parameters": { "type": "object", "properties": { ... }, "required": ["city"] },
  "terminal": false
}
```

- `terminal: true` 的工具调用成功后，工具循环**立即结束**、不再请求一次 LLM（`end_turn` 就是这么实现的）。一般工具不要开。
- 工具 handler 返回字符串作为 `role=tool` 内容；返回 `"error: ..."` 会被模型视为错误结果。
- 工具在 MindLoop Worker 上同步执行；长耗时操作会卡住整个回合（模型无法中途打断一个正在阻塞的工具）。
- 插件卸载/热重载时宿主按 owner 注销其工具，并等待在途调用计数归零。

**出站通道（`sdk::Channel`）**

`send()` 收到单条气泡的 JSON：

```json
{
  "source": "napcat",
  "channelId": "group:123456",
  "text": "你好喵",
  "replyTo": "可选：被引用的平台消息 ID",
  "mentions": ["可选：@ 用户 ID"],
  "parts": [{ "kind": "image", "data": "...", "mimeType": "image/png" }]
}
```

- `text` 已按 `[SEP]` 拆成多条气泡，`parts` 只挂在第一条气泡上；
- 宿主按 `source` 找通道；**找不到时会退化为使用第一个注册的通道**（历史兼容行为，建议始终注册与事件一致的 source）；
- 返回 `true` 表示发送成功；失败时写错误信息到 sink（C ABI 的 `sink`/`write`），宿主会记 `warn`；
- 通道发送发生在出站主链路上，**不要阻塞**；
- `capabilities()` 当前仅被 SDK 透传，宿主尚未消费，属于预留字段。

**Hook 三类**

| point | 载荷 | 返回值语义 |
|---|---|---|
| `on_event` | `Event` JSON（入站） | `drop=true` 丢弃；返回 JSON 替换事件；`nullopt` 放行 |
| `on_before_llm` | `{"messageCount": N}` | 不能改历史；返回 `{"extraSystemHint":"..."}` 会追加一条 `role=system` 消息到本次请求 |
| `on_outbound` | `{"source","channelId","text","replyTo","mentions"}` | `false`/`drop` 丢弃；返回 JSON 覆盖这四个字段（`parts` 不可改） |

- `priority` 越小越先执行，相同 priority 按注册顺序；
- Lua-host 在 `on_event`/`on_outbound` 上固定占用 priority 100，可用 `<100` 抢在 Lua 之前、`>100` 排在 Lua 之后；
- `on_before_llm` 只能走 Native。**不要用它在历史里插内容**——改历史会让已发送前缀失效，破坏 prompt cache。

### 3.7 事件与注意力层级

`postEvent` 接受 `Event` JSON（字段见 `include/core/event/Event.h`）：

```json
{
  "category": "ExternalMessage",
  "tier": "DirectFull",
  "source": "console",
  "channelId": "private:0d00",
  "actorId": "0d00",
  "title": "0d00",
  "content": "在吗",
  "timestamp": 1730000000000,
  "parts": [{ "kind": "image", "data": "data:image/png;base64,...", "mimeType": "image/png" }],
  "data": { "message_id": "平台消息ID（可选）", "reply_to": "平台回复ID（可选）" }
}
```

- `category`：`ExternalMessage` / `PluginNotice` / `InternalPulse` / `SystemSignal`；
- `tier`：
  - `DirectFull`：正文 + 附件全文注入上下文，并**唤醒**模型；
  - `Preview`：只注入事件头（来源/标题/时间），也唤醒模型；
  - `Digest`：进入攒批缓冲（默认满 10 条）才唤醒，之后按来源汇总条数注入；
  - `Silent`：只归档不注入、不唤醒（适合纯审计/埋点）。
- `id` 由宿主 AttentionGate 分配，插件不需要填；`timestamp` 建议毫秒（宿主也兼容秒级）。
- 所有事件都会写入 `Achieve` 归档；`parts[].data` 支持 `data:` URI 或 `file://绝对路径`，超过 `achieve.inlineMediaMaxBytes` 的内联媒体会被自动外置为 `file://`。

选 tier 的经验：**用户在跟你说话 → DirectFull；平台系统通知 → Preview；群聊围观 → Digest；日志/埋点 → Silent。**

### 3.8 配置、数据目录与热重载

- 构造/重配时传入的 JSON 就是 `config.json` 里该插件的 `config` 字段，原样透传；
- 数据目录：`host.dataDir()` → `<dataDir>/plugins/<id>/`（首次调用自动创建，适合放缓存与本地状态）；
- 热重载入口：

| 入口 | 行为 |
|---|---|
| `/reload` / `POST /api/reload` | 重载配置：重建 LLM/ContextBuilder/Persona，刷新 System Prompt；**不会重建插件** |
| `/plugin reload <id>` / `POST /api/plugins/reload` | 对该插件调 `reconfigure(new_config)`；失败则 stop→destroy→create→start |
| `/api/plugins` | 查看已加载插件、状态与各自工具 |

建议：`reconfigure()` 里**尽量就地更新配置**（换端口、换 token、改过滤规则），返回 `true`；只有结构性变化（换协议、换依赖对象）才返回 `false` 让宿主重启实例。

### 3.9 线程与资源清单

写 Native 插件时自查：

- [ ] `start()` 立即返回，所有阻塞工作在线程里；
- [ ] `stop()` 可重复调用，且 join 掉所有线程（不能 detach 后继续使用 host）；
- [ ] 回调（工具/hook/send）里没有长时间 `sleep`、没有同步网络超时；
- [ ] `post_event` 不在 `on_event` / `on_outbound` 回调里调用（会递归）；
- [ ] 插件内共享状态用 mutex/atomic，注意 stop 与回调的竞态；
- [ ] 不把 `mio_str` 的指针存起来（借用语义，回调返回即失效）；
- [ ] 异常不跨越 ABI（含 `std::bad_alloc`）。

---

## 4. 通用最佳实践

1. **工具描述写给模型看，不是写给人看**：说明什么时候该调用、输入格式、返回什么；schema 严格声明参数（宿主会过滤未声明参数）。
2. **不要重复造内置能力**：模型已有 `send_message`（主动发消息、`[SEP]` 分气泡）、`wait`、`keepsilent`（暂不回复但继续思考）、`end_turn`（结束回合）、`get_current_time`、`query_events`、`list_contacts/upsert_contact/set_notes`、注意力规则工具。插件应补足平台/设备/外部服务能力。
3. **控制刷新频率**：`refreshFacts()` 会重建 System Prompt，导致前缀缓存失效并触发历史压缩（见 MindLoop 的前缀缓存保护）——别在每轮对话里调用。
4. **日志 tag 用插件 id**：`host.info("myplugin", "...")`，Lua 用 `mio.log.info` 自动带 `lua:<id>`。排障时按 tag 过滤最快。
5. **多模态**：入站 `parts[].data` 用 `data:<mime>;base64,...`；超大媒体交给 Achieve 外置。出站 `parts` 只挂第一条气泡，通道自己决定怎么发。
6. **安全**：模型生成的参数一律不可信（路径拼接、命令执行、SQL）；Lua 沙箱已限制能力，Native 插件要自己做白名单。
7. **版本化**：`MIO_PLUGIN_ABI_VERSION` 不匹配的插件会被直接跳过（`PluginManager::loadAll` 有告警）。

---

## 5. 调试与排障

### 5.1 常用命令

```text
/help                         控制台帮助
/plugin list                  已加载插件与状态
/plugin reload lua:dice       热重载 Lua 脚本
/reload [path]                热重载配置
/state                        运行状态
/quit                         退出
```

```bash
curl http://127.0.0.1:6188/api/status
curl http://127.0.0.1:6188/api/plugins
curl -X POST http://127.0.0.1:6188/api/plugins/reload -d '{"id":"lua:dice"}'
```

### 5.2 常见问题

| 现象 | 可能原因 | 排查 |
|---|---|---|
| 插件完全没加载 | `config.plugins` 数组存在但没列该 id；`enabled=false`；ABI 版本不匹配；`MIO_PLATFORM` 覆盖 | 看启动日志 `已加载插件: <id>` / `ABI 版本不匹配` |
| 工具没出现在模型侧 | 注册失败（重名被覆盖、参数非法）、owner 被卸载、插件未 start | `/api/plugins` 看 `tools` 字段；检查 `registerTool` 返回值 |
| 出站消息发不出去 | 没有注册通道；`source` 与注册名不一致（会 fallback 到第一个通道，但日志里 source 对不上）；通道 send 返回失败 | 看 `OutboundRouter` 日志 |
| Hook 不生效 | hook point 拼写错误（只认 `on_event`/`on_before_llm`/`on_outbound`）；priority 被别的 Hook 抢先 drop；Lua 脚本没注册 `mio.on_event` | 启动日志 `已注册 Hook: point=...` |
| Lua 工具没反应 | 脚本报错（看 `lua:<id>` warn）；沙箱 OOM / 指令超限；脚本返回 `nil` | 日志 + 先在 `mio.log.info` 打点 |
| 热重载后工具旧行为 | 没 `/plugin reload`；Lua 是旧沙箱在跑；Native 插件是旧二进制 | 重启或重新编译；比对日志时间戳 |
| 插件访问数据目录失败 | 只有 Native 有 `dataDir`；Lua 无文件能力 | 见 §2.8 |
| 定时器不执行 | `mio.every` 最小 1 秒；脚本加载时未注册（写在 return 之后不会执行） | 把 `mio.every` 放在脚本主流程里 |

---

## 6. 仓库内参考实现

| 文件 | 看什么 |
|---|---|
| [plugins/lua/dice.lua](../plugins/lua/dice.lua) | 最小 Lua 工具插件 |
| [plugins/lua/dreaming.lua](../plugins/lua/dreaming.lua) | 定时自省插件：`mio.every` + `mio.local_time()` + `mio.post_event` 每日唤醒模型整理记忆 |
| [plugins/lua/src/LuaHostPlugin.cpp](../plugins/lua/src/LuaHostPlugin.cpp) | Lua 沙箱与 `mio.*` 绑定实现（限制、热重载、Hook 派发都在这里） |
| [plugins/console/src/ConsolePlugin.cpp](../plugins/console/src/ConsolePlugin.cpp) | 最小 Native 插件：REPL + 通道 + 管理命令 |
| [plugins/napcat/src/NapCatPlugin.cpp](../plugins/napcat/src/NapCatPlugin.cpp) | 真实平台适配器：WebSocket 服务、事件解析、媒体/ASR、工具、配置热更新 |
| [plugins/websearch/src/DeepSeekSearchPlugin.cpp](../plugins/websearch/src/DeepSeekSearchPlugin.cpp) | Native 插件接外部 HTTP API：cpr 调 DeepSeek Search/Fetch，注册 `web_search` / `web_fetch`，含 Key 校验、错误翻译、UTF-8 截断与输出预算（文档见 [deepseek-search.md](deepseek-search.md)） |
| [sdk/include/mio/plugin.hpp](../sdk/include/mio/plugin.hpp) | C++ SDK：`Host` / `Plugin` / `Channel` / `MIO_PLUGIN` |
| [sdk/include/mio/plugin_abi.h](../sdk/include/mio/plugin_abi.h) | C ABI 全量定义与 ABI 约定 |
| [src/plugin/PluginManager.cpp](../src/plugin/PluginManager.cpp) | 加载、Hook 派发、热重载、owner 清理 |
| [src/plugin/OutboundRouter.cpp](../src/plugin/OutboundRouter.cpp) | 出站拆气泡、打字延迟、通道路由 |
