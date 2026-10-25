# MIO（澪）

<div align="center">

**面向低成本 · 追求高拟人的现代化 C++17 聊天机器人框架**
**最新功能详见dev分支**
[![C++17](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Build Tool](https://img.shields.io/badge/Build-xmake-red.svg)](https://xmake.io)
[![Protocol](https://img.shields.io/badge/Protocol-OneBot%2011%20(NapCat)-green.svg)](https://github.com/NapNeko/NapCatQQ)
[![LLM Backend](https://img.shields.io/badge/LLM-OpenAI%20Compatible-orange.svg)](https://platform.openai.com/docs/api-reference)
[![Status](https://img.shields.io/badge/Status-Alpha%20Testing-yellow.svg)](#️-alpha-版本与风险提示)

</div>

---

> ⚠️ **项目状态与风险提示（Alpha 阶段）**
> 
> 本项目当前处于 **Alpha 早期实验与验证阶段**，核心架构和模块仍在持续迭代演进：
> - **接口与架构易变**：内部 API、上下文组织策略、长期记忆与图谱数据结构未来可能发生破坏性变更（Breaking Changes），暂不保证版本向后兼容性。
> - **未历经大规模生产验证**：MCP 外部工具、关系图谱及会话管理目前主要在小规模与本地测试环境下运行，尚未经过高并发、超大规模数据集的长期压测与生产验证。
> - **大模型与外部服务不确定性**：话题推断、公开/私密性判定、工具调用以及长期记忆读写依赖底层大模型与外部 MCP Server，不同模型和服务效果差异较大，可能出现判定偏差、幻觉或非预期工具调用。
> - **部署安全提醒**：使用 OneBot 11 反向 WebSocket 接入时，请务必配置访问令牌（Token），并妥善配置防火墙以限制网络暴露范围。测试期间请做好重要数据的定期备份。

---

## 📖 目录

- [💡 什么是 MIO？](#-什么是-mio)
- [✨ 核心特性](#-核心特性)
- [🏛 总体架构设计](#-总体架构设计)
- [🧠 关键技术机制深度解析](#-关键技术机制深度解析)
  - [1. Prompt Cache 前缀缓存优先设计](#1-prompt-cache-前缀缓存优先设计)
  - [2. 会话融合（Conversation Fusion）与跨平台身份归一](#2-会话融合conversation-fusion与跨平台身份归一)
  - [3. MCP 长期记忆与动态工具](#3-mcp-长期记忆与动态工具)
  - [4. 潜意识层与动态关系图谱（RelationshipGraph）](#4-潜意识层与动态关系图谱relationshipgraph)
  - [5. 工具循环与执行护栏（ToolLoop）](#5-工具循环与执行护栏toolloop)
- [📦 依赖与构建](#-依赖与构建)
- [🚀 快速开始与使用指南](#-快速开始与使用指南)
  - [1. 控制台交互模式（Console REPL）](#1-控制台交互模式console-repl)
  - [2. NapCatQQ (OneBot 11) 接入运行](#2-napcatqq-onebot-11-接入运行)
- [⚙️ 配置与环境变量参考](#️-配置与环境变量参考)
- [📂 源码目录结构（src/）](#-源码目录结构src)

---

## 💡 什么是 MIO？

**MIO（澪）** 是一个采用现代 **C++17** 构建的聊天机器人框架，旨在探索解决 AI 聊天机器人落地中的两大常见难题：

1. **成本控制（Low-Cost 导向）**：针对传统 Bot 粗暴拼接全量历史与上下文导致 Token 消耗过快的问题，MIO 尝试通过 **Prompt Cache 前缀命中优先**、**分级滚动摘要**、**MCP 长期记忆按需检索** 与 **稳定事实层注入**，在系统架构层面降低长会话的 Token 消耗与计算开销。
2. **拟人化与连续性（High-Anthropomorphism 导向）**：针对传统 Bot 在私聊与群聊之间信息割裂、不同平台身份分散的问题，MIO 设计了 **跨平台同人身份归一**、**关系图谱（亲密与信任衰减）**、**基于路由的跨会话动态融合（Fusion）** 以及 **MCP 长期记忆桥接**，尝试提升机器人的连续记忆感与社交交互体验。

---

## ✨ 核心特性

- ⚡ **Prompt Cache 前缀缓存优先**：
  - 上下文遵循三段式组织（稳定 System 前缀 + 追加式历史 + 当前回合），减少易变环境数据对前缀的破坏，提升大模型前缀缓存命中概率。
  - 每轮用户消息只追加稳定的 `[time:YYYY-MM-DD HH:MM]` 标签，历史部分逐字节不变；通讯录与认知变更折叠到明确的刷新边界，避免频繁重建 System Prompt。
- 🌐 **会话融合（Conversation Fusion 即路由）**：
  - **私聊按人归一**：同一用户在不同平台接入均映射至统一内部 ID，便于同人跨端同步上下文。
  - **跨会话自适应融合**：当两人熟悉度高、相互信任、当前话题相近且处于非私密状态时，系统可将相关上下文摘要追加重定向，探索打破群聊与私聊的信息壁垒。
- 🧩 **MCP 动态工具与长期记忆**：
  - 运行时通过 stdio JSON-RPC 启动并管理外部 MCP Server，自动发现工具并合并进 `ToolRegistry`，支持工具名前缀隔离。
  - 滚动摘要经后台 `SummaryBridge` 写入 MCP 长期记忆工具（如 `graphiti_add_memory`），不阻塞会话线程；仓库不再内置 SQLite 向量记忆。
- 📥 **输入缓冲与可靠回复**：
  - 对用户连续发送的多条消息做防抖聚合，同一轮只触发一次回复；支持代际打断，避免旧回合覆盖新回合。
  - 模型可通过 `send_message` 主动发送消息、用 `[SEP]` 拆分多气泡、按节奏调用 `wait`，或用 `keepsilent` 已读不回。
- 👥 **动态人际关系图谱（RelationshipGraph）**：
  - 节点包含用户和 MIO 自身，边记录亲密熟悉度（`intimacy`）与有向信任度（`trust`）。
  - 引入简单的时间衰减机制（$e^{-\Delta t/\tau}$），长时间未互动的对象在提示词注入名单中的排序自然回落。
  - 提供 `list_contacts` / `upsert_contact` / `set_notes` 通讯录工具；称呼变更工具 `set_nickname` 已停用，避免模型在轮次间漂移称呼。
- ⏰ **配置、维护任务与热重载**：
  - `config.json` 支持局部覆盖、C++ 风格注释、`--config` 指定路径与运行中热重载。
  - Scheduler 支持 `once` / `daily` / `interval` 维护任务：日界刷新 Facts，或对当天活跃会话执行隐藏维护回合整理长期记忆。
- 🛡 **基础 Agent 护栏机制**：
  - 工具调用参数白名单校验，过滤未声明的幻觉字段。
  - 支持同名同参连续调用检测、工具结果超限截断、步数耗尽强制总结等容错保护。
- 🔌 **主流模型与平台适配**：
  - 基于抽象 LLM 语义契约，兼容各类 OpenAI 兼容端点（如 DeepSeek、Qwen、小米 MiMo、SiliconFlow、Ollama、vLLM 等）。
  - 支持推理模型思维链（`reasoning_content`）透传与持久化。
  - 支持 OneBot 11 反向 WebSocket 协议（NapCatQQ），包含图片、语音、视频、文件处理与可选 ASR；同时保留本地控制台 REPL 交互。

---

## 🏛 总体架构设计

MIO 遵循 **“组合根（Runtime）统一协调，底层接口只暴露语义，模块边界分明”** 的设计思路：

```
                              ┌───────────────────────────────────┐
                              │           平台适配层              │
                              │  ConsoleAdapter / NapCat (OneBot) │
                              └─────────────────┬─────────────────┘
                                                │ IncomingMessage (事件流入)
                                                ▼
┌───────────────────────────────────────────────────────────────────────────────────────────────────┐
│ Runtime 组合根 (状态持有 / 上下文锁调度 / 线程同步)                                               │
│                                                                                                   │
│   1. 身份归一与路由 ─────────────────────────► FusionRouter (依据亲密/信任/话题/私密性决策融合)          │
│   2. 会话档案持久化 ────────────────────────► Achieve (每会话纯追加 jsonl 存储，无策略)               │
│   3. 上下文构建与组装 ──────────────────────► ContextBuilder (水位超限触发重新冷启动压缩)              │
│   4. 潜意识与事实层 ────────────────────────► Facts & RelationshipGraph (人设+Top-K人物+日记认知)     │
│   5. MCP 动态工具与长期记忆 ───────────────► McpManager / ToolRegistry / SummaryBridge              │
│   6. 工具循环与护栏 ────────────────────────► ToolLoop & ToolRegistry (白名单/死循环自愈/截断保护)   │
└───────────────────────────────────────────────┬───────────────────────────────────────────────────┘
                                                │ ChatRequest (标准化请求)
                                                ▼
                              ┌───────────────────────────────────┐
                              │            大模型适配层           │
                              │  OpenAiCompat (cpr 同步调用)       │
                              │  OpenAiEmbedding (向量化端点)     │
                              └─────────────────┬─────────────────┘
                                                │ Wire JSON
                                                ▼
                                   LLM API (OpenAI 兼容协议端点)
```

---

## 🧠 关键技术机制深度解析

### 1. Prompt Cache 前缀缓存优先设计

大模型 API 计费中，命中前缀缓存（Prompt Cache）的输入 Token 单价通常显著低于未缓存 Token。MIO 在上下文构建层进行了针对性设计：

- **三段式请求拓扑**：
  ```
  [ System (Facts) ]   人设 + 认识的人 (Top-K) + 封存日记认知（两次重建间逐字不变，作为缓存前缀基）
         │
  [ 历史对话流 ]        严格追加式增长；首条可为阶段性滚动摘要（isSummary=true）
         │
  [ 本轮回合与工具 ]    用户本轮发言 + 每条用户消息的稳定时间标签，承担新鲜上下文
  ```
- **避免在 System 中渲染高频易变数据**：
  - 不在 System Prompt 中直接硬编码实时时间（避免每次请求都破坏前缀缓存）；模型需要精确时间时可调用内置工具 `get_current_time`。
  - 不在 System 中渲染实时变化的消息计数或频繁波动的分数，仅渲染阶段性冻结的列表。
- **重构时机天然重合**：
  - 仅在会话**冷启动（Cold Start）** 或 **上下文超出 85% 水位触发滚动摘要** 时，才重新拉取人物权重并消费日记归档。此时由于截断前缀本身即发生改变，同步更新 System Prompt 不会带来额外的缓存惩罚。

### 2. 会话融合（Conversation Fusion）与跨平台身份归一

传统聊天机器人通常以 `(平台, 会话ID)` 作为硬性隔离，跨平台或私聊与群聊间信息完全阻断。

```
[用户 A (QQ私聊)]   ──┐
                      ├──► RelationshipGraph.onSeen ──► 归一至 internalId
[用户 A (Console)]  ──┘                                        │
                                                               ▼
                                                  私聊统一接入同一 FusionUnit
```

- **路由判决（Can Fuse?）**：
  当不同来源的消息到达时，`FusionRouter` 综合以下指标判定是否融合：
  1. **熟悉度（Intimacy）**：私聊↔群聊时考察人物是否在该群；私聊↔私聊考察亲密熟悉度（阈值可配）。
  2. **信任度（Trust）**：考察关系图谱中的有向信任边（阈值可配）。
  3. **话题语义相关性（Topic Semantic Matching）**：
     - 上下文具备动态 `topic` 属性。
     - 若配置了向量服务，Router 将双侧 topic 向量化（带简单缓存），余弦相似度达到阈值时视为话题相近；若未配置向量服务则回退至精确字符匹配。
  4. **隐私状态（Privacy Policy）**：冷启动默认私密，双方上下文必须均处于公开状态（`isPublic == true`）方可融合。模型可通过内置工具 `set_public` 切换上下文私密属性。
- **融合执行**：
  触发融合时，系统将短上下文提取摘要后重定向追加至目标上下文末尾。

### 3. MCP 长期记忆与动态工具

MIO 将长期记忆和大量外围能力交给 **MCP（Model Context Protocol）**，自身只保留稳定的会话、上下文与工具编排职责：

```
[外部 MCP Server]  graphiti / filesystem / search / 自定义服务 ...
       │  stdio JSON-RPC
       ▼
McpManager ──► McpClient ──► ToolRegistry (工具名前缀隔离)
                                  │
                                  ├─► ToolLoop：模型按需调用
                                  │
                                  └─► SummaryBridge：滚动摘要直接写入长期记忆
```

- **动态挂载**：`mcp.servers` 里的每个 Server 由子进程启动，通过 stdio JSON-RPC 完成初始化、工具发现与调用；`toolPrefix` 可避免外部工具与内置工具重名。
- **摘要写入链路**：冷读取、融合重定向与重新冷启动产生的累计摘要，会在 ingest 线程中只做入队；后台 `SummaryBridge` 调用 `memoryBridge.tool` 指定的 MCP 工具（默认 `graphiti_add_memory`）。每个会话只保留最新一条待写入摘要，相同文本不会重复写。
- **参数映射**：`memoryBridge.textField` 指定摘要文本的字段名；`extraArgs` 中的字符串支持 `{text}`、`{topic}`、`{conv}`、`{group}`、`{private}` 占位符，适配不同 MCP 记忆服务的参数结构。
- **按需检索**：长期记忆的查询、事实搜索与节点搜索同样由 MCP 工具暴露给模型，由模型根据对话意图主动调用，而不是每轮无差别注入。
- **迁移提示**：内置 SQLite 记忆与 `recall_memory` / `remember` 工具已移除，旧 `data/memory.db` 不会自动迁移；需要保留历史记忆时，请在升级前自行导出并写入外部 MCP 服务。

### 4. 潜意识层与动态关系图谱（RelationshipGraph）

MIO 在短期上下文之外维护轻量的事实层：

- **节点（PersonNode）**：包含内部 ID、各平台映射、模型选定称呼、印象、喜好及随时间衰减的互动权重。MIO 自身也作为节点存在。
- **关系边（RelationshipEdge）**：记录对称亲密熟悉度（`intimacy`）与有向信任度（`trust`）。
- **时间衰减**：
  $$\text{Weight} = \sum e^{-\frac{\Delta t}{\tau}} \quad (\tau \approx 3\text{天})$$
  长时间缺乏互动的对象权重自然回落，从 Top-K 注入名单中淡出，但图谱节点与历史数据依然保留，重新互动时可再次激活。
- **事实刷新边界**：System Prompt 只在校启动、配置重载、`refresh_facts` 定时任务或重新冷启动时重建；通讯录变更只置脏标记，留到刷新边界统一消费，保护 prompt cache 前缀。

### 5. 工具循环与执行护栏（ToolLoop）

MIO 提供基础的 Agent 执行闭环与保护护栏：

| 内置护栏 | 触发场景 | 保护动作 |
|---|---|---|
| **参数 Schema 白名单** | 模型生成了未在工具中声明的虚假参数 | 过滤非法键，仅保留 JSON Schema 声明的有效字段 |
| **未知工具名提示** | 模型调用了不存在的工具 | 不中断会话，将可用工具列表提示回传给模型以供自纠 |
| **重复调用拦截** | 同名同参数工具连续调用达到阈值（如 3 次） | 注入提示警告，强制模型改变思考方向 |
| **超大结果截断** | 工具返回文本超出预设上限（如 4KB） | 结果落盘保存，仅向模型回传摘要预览及路径 |
| **步数耗尽保护** | 往返轮数达到上限（默认 6 步） | 临时剥离工具定义，注入总结指令促使模型完成回答 |

#### 🔧 内置工具列表

> 长期记忆（存取/召回）由外部 MCP Server 提供（如 `graphiti_add_memory`
> / `graphiti_search_memory_facts` / `graphiti_search_nodes`）；内置的
> `remember` / `recall_memory` 已随 SQLite 记忆系统移除。MCP 工具会按配置自动追加到工具表。

| 工具名称 | 参数 | 职责说明 |
|---|---|---|
| `get_current_time` | `{}` | 获取当前本地日期与时间（精确到分钟），避免在 System 中写死时间 |
| `set_public` | `{"is_public": bool, "reason": string}` | 根据对话敏感程度标记当前上下文是否允许融合 |
| `update_topic` | `{"topic": string}` | 上报当前对话主题，为会话融合提供语义判定依据 |
| `get_current_user_qq` | `{}` | 获取当前对话用户的 QQ 号、昵称及会话信息 |
| `list_contacts` | `{"query"?: string}` | 查询通讯录（按称呼 / QQ / internal_id，缺省列出全部联系人） |
| `upsert_contact` | `{"name": string, "qq"?: string, "platform"?: string, "user_id"?: string, "notes"?: string}` | 登记新联系人；已登记账号不会被覆盖称呼 |
| `set_notes` | `{"person": string, "notes": string}` | 更新某人的特点、喜好或印象备注 |
| `send_message` | `{"text": string, "target"?: string, "mention_user_ids"?: string[], "reply_to_message_id"?: string}` | 主动发送消息，`text` 中可用 `[SEP]` 拆成多气泡，支持 QQ 群聊 @ 与引用 |
| `wait` / `sleep` | `{"seconds": number, "reason"?: string}` | 等待指定秒数后继续思考，用于模拟真人节奏 |
| `keepsilent` / `keep_silent` / `stay_silent` | `{"reason": string}` | 保持沉默、已读不回 |

> `set_nickname` 已停用：模型自行改称呼会造成跨轮次身份漂移；底层能力保留在注释中，后续可评估恢复。

#### 🕒 每轮对话的时间标签

wire 层会给**每一轮**用户消息附一个 `[time:YYYY-MM-DD HH:MM]`（取消息自己的
`createdAt`，所以历史部分逐字节稳定、不会打断 prompt cache）。同一个输入缓冲区
（同一轮）里的多条消息共享时间戳，只给本轮第一条打标签。

#### ⏰ 定时事项（维护任务）

`config.json` 的 `maintenance` 段是一张通用任务表。设计前提是**缓存前缀优先**：
`systemPrompt` 位于每次请求的第一位，改一个字节就会让**所有**会话的整段前缀缓存失效，
所以通讯录/认知变更不会立刻进 prompt，而是折叠到日界统一消费。

| 字段 | 说明 |
|---|---|
| `enabled` | 总开关 |
| `tasks[].action` | `refresh_facts`（消费日记 + 重建 Facts）/ `prompt`（维护回合） |
| `tasks[].kind` | `daily`（每天 `at`）/ `once`（`at` 执行一次）/ `interval`（每 `everyMinutes` 分钟） |
| `tasks[].at` | 本地 `HH:MM`（`daily` / `once`） |
| `tasks[].prompt` | `action=prompt` 时的提示文本 |

**滚动摘要 → MCP 长期记忆**：`memoryBridge` 段把每次摘要产出（冷读取 / 融合重定向 /
重新冷启动）经后台线程直接写入 MCP 记忆工具（默认 `graphiti_add_memory`，参数名用
`textField` 指定，其余固定参数放 `extraArgs`，字符串值支持 `{text}` `{topic}` `{conv}`
`{group}` `{private}` 占位符）。摘要 sink 只入队不阻塞（它在 ingest 线程里、常持锁），
每个会话只保留最新一条累计摘要，同一份文本不会重复写，工具不在工具表里会告警一次后跳过。

`refresh_facts` 是 Facts 的常规刷新边界（另外两个边界是配置重载与重新冷启动）。
`prompt` 会对"当天有活动的每个会话"各跑一次**隐藏回合**：模型可以调用长期记忆写入
工具（长期记忆由 MCP 提供，如 `graphiti_add_memory`）把当天的经历写进记忆，但
`send_message` 会被拒绝、输出不落档案、也不污染真实会话历史（只在上下文副本上工作）。日志里 `systemPrompt 重建` 一行带 `fp` 指纹，
`fp` 不变就说明缓存前缀没被打断。示例见 `config.example.json` 第 11 节。

---

## 📦 依赖与构建

### 1. 环境准备

- **操作系统**：Linux / macOS / Windows
- **C++ 编译器**：支持 **C++17** 的现代编译器（GCC 8+, Clang 7+, MSVC 2019+）
- **构建工具**：[xmake](https://xmake.io/)
- **可选运行依赖**：如果需要使用 MCP 工具，请准备对应 MCP Server 的运行环境（如 Python、Node.js/npx）及服务配置。

C++ 依赖（`cpr`、`nlohmann_json`、`ixwebsocket`）均由 `xmake.lua` 自动拉取与配置。项目已不再依赖 `sqlite3`。

### 2. 构建命令

```bash
# 1. 配置项目（Release 模式）
xmake f -m release

# 2. 编译项目
xmake

# 产物默认位于：
# ./build/linux/x86_64/release/MIO  (Linux)
# 或 ./build/windows/x64/release/MIO.exe (Windows)
```

调试模式构建：
```bash
xmake f -m debug
xmake
```

---

## 🚀 快速开始与使用指南

### 1. 控制台交互模式（Console REPL）

推荐从完整样板复制配置文件，按需修改模型、平台与工具设置：

```bash
# 1. 生成配置文件
cp config.example.json config.json

# 2. 至少修改 openai.baseUrl / openai.apiKey / openai.model
#    可选：platform、napcat、mcp、maintenance、memoryBridge

# 3. 构建并以配置文件启动
xmake f -m release
xmake
./build/linux/x86_64/release/MIO --config config.json
```

也可以直接使用 `xmake run MIO -- --config config.json`，或通过 `MIO_CONFIG_PATH` 指定配置文件。未提供配置文件时，程序会回退到环境变量配置。

#### 🕹 控制台支持指令：

| 指令 | 示例与参数说明 |
|---|---|
| `/chat <内容>` | `/chat 你好呀！`（简易私聊：固定发给模拟用户 `0d00`） |
| `/msg <类型> <会话ID> <发送者ID> <内容>` | `/msg private 10001 userA 吃了没？`<br>`/msg group 20001 userB 大家好` |
| `/state` | 打印当前运行态快照（已处理消息数、活跃会话、活跃用户等） |
| `/reload [path]` | 热重载配置；不传 path 时读取 `config.json` |
| `/help` | 查看控制台指令帮助 |
| `/quit` | 退出程序 |

---

### 2. NapCatQQ (OneBot 11) 接入运行

MIO 原生支持通过 **OneBot 11** 反向 WebSocket 接入 [NapCatQQ](https://github.com/NapNeko/NapCatQQ)：

#### ① NapCat 侧配置（反向 WebSocket 客户端）
在 NapCat 的 `onebot11_<QQ号>.json` 配置文件中，找到 `websocketClients` 字段，添加配置。`url` 的端口必须与 MIO 的 `napcat.listenPort` 一致（样板默认 `8082`）：
```json
{
  "enable": true,
  "url": "ws://127.0.0.1:8082",
  "reportSelfMessage": false,
  "messagePostFormat": "array",
  "token": "your_secure_token"
}
```

#### ② 配置并启动 MIO
在 `config.json` 中启用 NapCat 平台，并填写与 NapCat 相同的 Token：

```json
{
  "platform": "napcat",
  "napcat": {
    "listenHost": "0.0.0.0",
    "listenPort": 8082,
    "token": "your_secure_token"
  }
}
```

```bash
./build/linux/x86_64/release/MIO --config config.json
```

入站图片、语音、视频与文件的处理开关、缓存目录和 ASR 参数位于 `config.example.json` 第 8 节 `napcat.media`。`MIO_PLATFORM` 环境变量仍可临时覆盖配置文件中的平台选择。

---

## ⚙️ 配置与环境变量参考

### 1. 配置文件

MIO 以 `config.json` 为主配置来源，完整样板见 [`config.example.json`](config.example.json)。支持通过以下方式指定：

```bash
./MIO --config /path/to/config.json
./MIO -c /path/to/config.json
MIO_CONFIG_PATH=/path/to/config.json ./MIO
```

配置文件支持 C++ 风格 `//` 注释；未填写的字段自动使用默认值，热重载时只覆盖 JSON 中出现的字段。主要配置段：

| 配置段 | 作用 |
|---|---|
| 顶层字段 | 机器人名称、人设、数据目录、平台、Admin 端口等 |
| `openai` | OpenAI 兼容对话端点、模型、采样参数、超时与重试 |
| `embedding` | 话题语义匹配使用的向量端点；未配置时可回退到 `openai` |
| `inputBuffer` | 输入防抖、最大聚合等待、批大小与文本上限 |
| `fusion` | 亲密度、信任度与话题相似度融合阈值 |
| `contextBuilder` | Token 预算、压缩水位与话题预热轮数 |
| `napcat` | 反向 WebSocket 监听、Token 与富媒体/ASR 配置 |
| `splitter` | `[SEP]` 多气泡分段与拟人打字延迟 |
| `mcp` | 外部 MCP Server 列表、启动命令、工具前缀与超时 |
| `maintenance` | `once` / `daily` / `interval` 维护任务表 |
| `memoryBridge` | 滚动摘要写入哪个 MCP 长期记忆工具及参数字段 |

内置 Admin Server 默认监听 `adminPort`（样板为 `6188`），提供：

| 方法 | 路径 | 作用 |
|---|---|---|
| `GET` | `/api/status` | 查看运行状态 |
| `GET` | `/api/config` | 获取当前配置快照 |
| `POST` | `/api/config` | 提交 JSON 局部更新并热重载 |
| `POST` | `/api/reload` | 从 `config.json` 重新加载 |

### 2. 环境变量回退

设置 `MIO_NO_CONFIG=1`，或当前目录不存在 `config.json` 且未指定 `--config` 时，程序会回退到环境变量配置。常用变量如下：

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `MIO_CONFIG_PATH` | *(空)* | 配置文件路径；存在时优先加载 |
| `MIO_NO_CONFIG` | *(空)* | 非空时强制使用环境变量配置 |
| `MIO_BOT_NAME` | `Mio` | 机器人名称 |
| `MIO_DATA_DIR` | `data` | 会话档案与运行数据目录 |
| `MIO_PLATFORM` | `console` | 运行平台：`console` 或 `napcat` |
| `MIO_BASE_URL` | `http://127.0.0.1:8080` | OpenAI 兼容对话端点 |
| `MIO_API_KEY` | *(空)* | 对话 API 密钥 |
| `MIO_MODEL` | `local-model` | 对话模型名称 |
| `MIO_MAX_TOKENS` | `512` | 单次生成最大 Token 数 |
| `MIO_MODEL_REASONING_EFFORT` | *(空)* | 推理努力程度：`low` / `medium` / `high` / `xhigh` / `max` |
| `MIO_THINKING` | *(空)* | 思考模式：`enabled` / `disabled` |
| `MIO_LLM_BACKEND` | `openai` | LLM 协议后端 |
| `MIO_EMBED_BASE_URL` | 回退到 `MIO_BASE_URL` | 向量化服务地址 |
| `MIO_EMBED_API_KEY` | 回退到 `MIO_API_KEY` | 向量化 API 密钥 |
| `MIO_EMBED_MODEL` | `BAAI/bge-m3` | 向量化模型名称 |
| `MIO_NAPCAT_HOST` | `127.0.0.1` | 反向 WebSocket 监听地址 |
| `MIO_NAPCAT_PORT` | `6199` | 反向 WebSocket 监听端口 |
| `MIO_NAPCAT_TOKEN` | *(空)* | OneBot 11 Access Token |
| `MIO_MEDIA_ENABLED` | `true` | 是否启用入站媒体处理 |
| `MIO_MEDIA_CACHE_DIR` | `data/media` | 媒体缓存目录 |
| `MIO_MEDIA_MAX_BYTES` | `8388608` | 单文件大小上限 |
| `MIO_ASR_ENABLED` | `false` | 是否启用语音转写 |
| `MIO_ASR_BASE_URL` | 回退到 `MIO_BASE_URL` | ASR 服务地址 |
| `MIO_ASR_API_KEY` | 回退到 `MIO_API_KEY` | ASR API 密钥 |
| `MIO_ASR_MODEL` | `whisper-1` | ASR 模型名称 |
| `MIO_ASR_LANGUAGE` | *(空)* | 识别语言，留空自动识别 |
| `MIO_LOG_LEVEL` | `info` | 日志级别：`debug` / `info` / `warn` / `error` |

> 配置文件模式是推荐用法；环境变量回退主要用于容器、本地调试或临时覆盖。

---

## 📂 源码目录结构（src/）

```
src/
├── adapters/                     # 平台适配器
│   ├── console/                  # 控制台 REPL 交互实现 (ConsoleAdapter)
│   └── napcat/                   # OneBot 11 反向 WS、消息解析、富媒体下载与 ASR
├── admin/                        # 内置 HTTP 管理服务 (AdminServer)
├── config/                       # 配置加载与热重载
│   ├── ConfigManager.*           # 线程安全配置快照与文件/环境重载
│   ├── AppConfig.*               # 顶层配置模型
│   ├── asr/                      # ASR 配置
│   ├── embedding/                # 向量嵌入配置
│   ├── mcp/                      # MCP Server 配置
│   └── openai/                   # OpenAI 兼容端点配置
├── context/                      # 上下文管理与装配
│   ├── achieve/                  # 会话历史冷读取与落盘 (Achieve)
│   ├── contextBuilder/           # 请求上下文装配与水位监控压缩 (ContextBuilder)
│   ├── conversationFusion/       # 会话融合路由与单元 (FusionRouter, FusionContext)
│   ├── costEstimator/            # Token 估算与安全截断 (Tokens)
│   ├── inputBuffer/              # 输入防抖、批处理与代际控制 (InputBuffer)
│   └── summarizor/               # 经历滚动摘要器 (SummaryManager)
├── core/                         # 核心领域实体
│   ├── conversation/             # 会话标识定义 (ConversationKey)
│   ├── event/                    # 领域事件定义 (Event)
│   ├── eventlog/                 # 事件审计日志存储 (EventLog)
│   ├── math/                     # 向量数学工具 (VectorMath)
│   ├── message/                  # 中性消息模型与历史视图 (Msg, Role, History)
│   └── time/                     # 本地时间工具 (LocalTime)
├── diary/                        # 封存式认知条目存储 (Diary)
├── log/                          # 格式化日志输出 (Log)
├── mind/                         # 潜意识与事实层
│   ├── facts/                    # 稳定 System Prompt 组装 (Facts)
│   └── graph/                    # 人际动态关系图谱 (RelationshipGraph)
├── providers/                    # 外部能力抽象与实现
│   ├── asr/                      # 语音识别抽象与 OpenAI 兼容实现
│   ├── embedding/                # 向量化抽象与 OpenAI 兼容实现
│   ├── llm/                      # LLM 抽象、OpenAI 兼容客户端、工具循环
│   │   ├── openai/               # Chat Completions / 推理内容 / 多模态请求
│   │   └── tool/                 # ToolRegistry 与 ToolLoop 护栏
│   ├── mcp/                      # MCP 客户端、Server 管理、JSON-RPC 与 stdio 传输
│   └── memory/                   # 长期记忆 Provider 接口（由外部实现）
├── runtime/                      # 运行时组合根
│   ├── Runtime.*                 # 状态、生命周期、上下文锁与工具接线
│   ├── ContactTools.*            # 通讯录工具
│   ├── MessageTools.*            # send_message / 多气泡发送
│   └── SummaryBridge.*           # 滚动摘要 → MCP 长期记忆
├── scheduler/                    # 定时任务调度器 (Scheduler)
└── main.cpp                      # 应用程序入口
```
