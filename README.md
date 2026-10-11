# MIO（澪）

<div align="center">

**面向低成本 · 追求高拟人的现代化 C++17 聊天机器人框架**

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
> - **未历经大规模生产验证**：当前本地 SQLite 向量检索、关系图谱及会话管理主要在小规模与本地测试环境下运行，尚未经过高并发、超大规模数据集的长期压测与生产验证。
> - **大模型输出不确定性**：日记提炼、话题推断、公开/私密性判定以及记忆检索依赖底层大模型的理解与遵循能力，不同模型效果差异较大，可能出现判定偏差、幻觉或非预期工具调用。
> - **部署安全提醒**：使用 OneBot 11 反向 WebSocket 接入时，请务必配置访问令牌（Token），并妥善配置防火墙以限制网络暴露范围。测试期间请做好重要数据的定期备份。

---

## 📖 目录

- [💡 什么是 MIO？](#-什么是-mio)
- [✨ 核心特性](#-核心特性)
- [🏛 总体架构设计](#-总体架构设计)
- [🧠 关键技术机制深度解析](#-关键技术机制深度解析)
  - [1. Prompt Cache 前缀缓存优先设计](#1-prompt-cache-前缀缓存优先设计)
  - [2. 会话融合（Conversation Fusion）与跨平台身份归一](#2-会话融合conversation-fusion与跨平台身份归一)
  - [3. 两段式长期记忆系统（SELECT & RECALL）](#3-两段式长期记忆系统select--recall)
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

1. **成本控制（Low-Cost 导向）**：针对传统 Bot 粗暴拼接全量历史与上下文导致 Token 消耗过快的问题，MIO 尝试通过 **Prompt Cache 前缀命中优先**、**轻量级本地嵌入与向量匹配**、**按需自主检索** 与 **分级滚动摘要**，在系统架构层面降低长会话的 Token 消耗与计算开销。
2. **拟人化与连续性（High-Anthropomorphism 导向）**：针对传统 Bot 在私聊与群聊之间信息割裂、不同平台身份分散的问题，MIO 设计了 **跨平台同人身份归一**、**关系图谱（亲密与信任衰减）**、**基于路由的跨会话动态融合（Fusion）** 以及 **认知日记沉淀**，尝试提升机器人的连续记忆感与社交交互体验。

---

## ✨ 核心特性

- ⚡ **Prompt Cache 前缀缓存优先**：
  - 上下文遵循三段式组织（稳定 System 前缀 + 追加式历史 + 当前回合），减少易变环境数据对前缀的破坏，提升大模型前缀缓存命中概率。
  - 会话超出设定水位线后触发增量滚动摘要与日记消费，前缀重构与摘要时机结合。
- 🌐 **会话融合（Conversation Fusion 即路由）**：
  - **私聊按人归一**：同一用户在不同平台接入均映射至统一内部 ID，便于同人跨端同步上下文。
  - **跨会话自适应融合**：当两人熟悉度高、相互信任、当前话题相近且处于非私密状态时，系统可将相关上下文摘要追加重定向，探索打破群聊与私聊的信息壁垒。
- 🧠 **两段式长期记忆系统（SELECT & RECALL）**：
  - **SQLite 原生向量存储**：无需引入独立的重型向量数据库服务，将向量序列化为 BLOB 与摘要文本共同落库。
  - **两段式检索流程**：第一阶段在 SQLite 层面通过元数据索引进行权限过滤预裁剪；第二阶段在内存通过 **C++ SIMD 硬件加速（AVX-512 / AVX2 / SSE2 / NEON 编译期分派）** 计算余弦点积与时间衰减得分。
  - **模型自主按需召回**：注册为 `recall_memory` 工具，由模型根据对话意图自主发起历史检索，避免每轮无差别盲目注入。
- 👥 **动态人际关系图谱（RelationshipGraph）**：
  - 节点包含用户和 MIO 自身，边记录亲密熟悉度（`intimacy`）与有向信任度（`trust`）。
  - 引入简单的时间衰减机制（$e^{-\Delta t/\tau}$），长时间未互动的对象在提示词注入名单中的排序自然回落。
- 🛡 **基础 Agent 护栏机制**：
  - 工具调用参数白名单校验，过滤未声明的幻觉字段。
  - 支持同名同参连续调用检测、工具结果超限截断、步数耗尽强制总结等容错保护。
- 🔌 **主流模型与平台适配**：
  - 基于抽象 LLM 语义契约，兼容各类 OpenAI 兼容端点（如 DeepSeek、Qwen、小米 MiMo、SiliconFlow、Ollama、vLLM 等）。
  - 支持推理模型思维链（`reasoning_content`）透传与持久化。
  - 支持 OneBot 11 反向 WebSocket 协议（NapCatQQ），并内置本地控制台 REPL 交互。

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
│   5. 长期记忆两段式检索 ────────────────────► MemoryManager (SQLite 预过滤 + 内存 SIMD 点积 + 衰减)   │
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
  [ 本轮回合与工具 ]    用户本轮发言 + 间隔 ≥30min 时的轻量时间戳，承担新鲜上下文
  ```
- **避免在 System 中渲染高频易变数据**：
  - 不在 System Prompt 中直接硬编码实时时间（避免每次请求都破坏前缀缓存）；模型需要获取时间时，由模型调用内置工具 `get_current_time`。
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

### 3. 两段式长期记忆系统（SELECT & RECALL）

针对每轮无脑拼接 RAG 消耗 Token 且容易稀释注意力的问题，MIO 采用 **“模型主动召回 + 两段式混合检索”** 的尝试性方案：

```
[写入流]
冷读取 / 融合重定向 / 重新冷启动 产出摘要
   │
   ▼
SummaryManager.onSummary 统一入口
   │
   ▼ 归属解析 (私聊归内部 ID / 群聊按会话共享)
Embedding 向量化 + L2 归一化
   │
   ▼ 原生 4096 字节 BLOB
INSERT OR IGNORE INTO memories (conv_key, summary 幂等去重)

[检索流]
模型主动调用 recall_memory(query)
   │
   ├─► 第一段：SQLite 索引预过滤 (WHERE is_public=1 OR person_id=:viewer OR conv_key=:conv)
   │         按权限裁剪出候选条目 (LIMIT ≤ 256)
   │
   └─► 第二段：C++ 内存 SIMD 运算 (AVX-512 / AVX2 / SSE2 / NEON 硬件加速点积)
             计算余弦相似度 × 时间衰减 exp(-Δt / τ) 复合得分
             Top-K 结果以 Tool 消息回喂给模型
```

- **容错降级机制**：若向量端点出现网络超时或错误，记忆系统设计有冷却机制，60 秒内不再频繁发起无效请求，尽量避免辅助记忆模块故障直接打断主聊天流程。

### 4. 潜意识层与动态关系图谱（RelationshipGraph）

MIO 在短期上下文之外维护轻量的事实层：

- **节点（PersonNode）**：包含内部 ID、各平台映射、模型选定称呼、印象、喜好及随时间衰减的互动权重。MIO 自身也作为节点存在。
- **关系边（RelationshipEdge）**：记录对称亲密熟悉度（`intimacy`）与有向信任度（`trust`）。
- **时间衰减**：
  $$\text{Weight} = \sum e^{-\frac{\Delta t}{\tau}} \quad (\tau \approx 3\text{天})$$
  长时间缺乏互动的对象权重自然回落，从 Top-K 注入名单中淡出，但图谱节点与历史数据依然保留，重新互动时可再次激活。
- **认知日记（Diary）**：模型在对话中通过 `write_diary` 记录阶段性事实或认知，日记在内存中缓冲，仅在摘要重构时批量消费并归档。

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

| 工具名称 | 参数 | 职责说明 |
|---|---|---|
| `get_current_time` | `{}` | 获取当前本地时间（精确到分钟），避免在 System 中写死时间 |
| `remember` | `{"text": string, "is_public"?: bool}` | 将具有长期价值的重要事实或约定存入长期记忆库 |
| `recall_memory` | `{"query": string, "top_k"?: int}` | 面对旧事或历史背景时，主动发起记忆检索 |
| `set_nickname` | `{"current": string, "nickname": string}` | 更新对某人的称呼（仅修改展示昵称，不改变底层映射） |
| `set_notes` | `{"person": string, "notes": string}` | 补充对某人的特点、喜好或印象描述 |
| `set_public` | `{"is_public": bool, "reason": string}` | 模型根据对话敏感程度，标记当前上下文是否允许融合 |
| `update_topic` | `{"topic": string}` | 上报当前对话主题，为会话融合提供语义判定依据 |
| `get_current_user_qq` | `{}` | 获取当前对话用户的 QQ 号、昵称及会话信息 |
| `get_known_person_qq` | `{"name"?: string}` | 获取认识的人的 QQ 号（支持按人名检索或列出所有已知人物） |

---

## 📦 依赖与构建

### 1. 环境准备

- **操作系统**：Linux / macOS / Windows
- **C++ 编译器**：支持 **C++17** 的现代编译器（GCC 8+, Clang 7+, MSVC 2019+）
- **构建工具**：[xmake](https://xmake.io/)
- **系统依赖**：`sqlite3`（包含头文件与库）
  - Ubuntu/Debian: `sudo apt install libsqlite3-dev`
  - CentOS/RHEL: `sudo yum install sqlite-devel`
  - Arch Linux: `sudo pacman -S sqlite`
  - Windows: 安装系统 sqlite3 或由 xmake 管理

其他依赖（`cpr`、`nlohmann_json`、`ixwebsocket`）均由 `xmake.lua` 自动拉取与配置。

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

本地测试可直接运行控制台交互界面，配置 OpenAI 兼容接口即可运行：

```bash
# 配置大模型端点（以 DeepSeek 或本地 Ollama 为例）
export MIO_BASE_URL="https://api.deepseek.com/v1"
export MIO_API_KEY="sk-xxxxxxxxxxxxxxxxxxxxxxxx"
export MIO_MODEL="deepseek-chat"

# 启动 MIO
xmake run MIO
```

#### 🕹 控制台支持指令：

| 指令 | 示例与参数说明 |
|---|---|
| `/chat <内容>` | `/chat 你好呀！`（简易私聊：固定发给模拟用户 `0d00`） |
| `/msg <类型> <会话ID> <发送者ID> <内容>` | `/msg private 10001 userA 吃了没？`<br>`/msg group 20001 userB 大家好` |
| `/state` | 打印当前运行态快照（已处理消息数、活跃会话、活跃用户等） |
| `/help` | 查看控制台指令帮助 |
| `/quit` | 退出程序 |

---

### 2. NapCatQQ (OneBot 11) 接入运行

MIO 原生支持通过 **OneBot 11** 反向 WebSocket 接入 [NapCatQQ](https://github.com/NapNeko/NapCatQQ)：

#### ① NapCat 侧配置（反向 WebSocket 客户端）
在 NapCat 的 `onebot11_<QQ号>.json` 配置文件中，找到 `websocketClients` 字段，添加配置：
```json
{
  "enable": true,
  "url": "ws://127.0.0.1:6199",
  "reportSelfMessage": false,
  "messagePostFormat": "array",
  "token": "your_secure_token"
}
```

#### ② 启动 MIO
指定平台为 `napcat` 并配置监听端口与 Token：

```bash
# 启用 NapCat 平台适配器
export MIO_PLATFORM=napcat

# 大语言模型配置
export MIO_BASE_URL="https://api.deepseek.com/v1"
export MIO_API_KEY="sk-xxxxxxxxxxxxxxxxxxxxxxxx"
export MIO_MODEL="deepseek-chat"

# 向量模型配置（可选，未配置时自动回退至 MIO_BASE_URL）
export MIO_EMBED_BASE_URL="https://api.siliconflow.cn/v1"
export MIO_EMBED_API_KEY="sk-xxxxxxxxxxxxxxxxxxxxxxxx"
export MIO_EMBED_MODEL="BAAI/bge-m3"

# NapCat 反向 WebSocket 服务端监听配置
export MIO_NAPCAT_HOST="0.0.0.0"
export MIO_NAPCAT_PORT=6199
export MIO_NAPCAT_TOKEN="your_secure_token"

# 运行 MIO
xmake run MIO
```

---

## ⚙️ 配置与环境变量参考

所有环境变量均有默认配置，可按需覆盖：

### 1. LLM 核心配置

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `MIO_BASE_URL` | `http://127.0.0.1:8080` | OpenAI 兼容服务地址（填至 `/v1` 之前即可） |
| `MIO_API_KEY` | *(空)* | API 密钥 |
| `MIO_MODEL` | `local-model` | 模型名称（如 `deepseek-chat`, `mimo-v2.5`） |
| `MIO_MAX_TOKENS` | `512` | 单次生成最大 Token 数上限 |
| `MIO_MODEL_REASONING_EFFORT` | *(空)* | 推理努力程度（`low`, `medium`, `high`） |
| `MIO_THINKING` | *(空)* | 思考输出开关（`enabled` / `disabled`） |
| `MIO_LLM_BACKEND` | `openai` | 大模型协议类型（当前实现兼容 `openai` 接口） |

### 2. 向量嵌入（Embedding）与记忆配置

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `MIO_EMBED_BASE_URL` | 回退到 `MIO_BASE_URL` | 向量化服务地址（支持独立第三方服务） |
| `MIO_EMBED_API_KEY` | 回退到 `MIO_API_KEY` | 向量化 API 密钥 |
| `MIO_EMBED_MODEL` | `BAAI/bge-m3` | 向量化模型名称（默认 1024 维） |

### 3. 平台适配器配置

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `MIO_PLATFORM` | `console` | 运行平台：`console`（控制台）或 `napcat`（NapCatQQ） |
| `MIO_NAPCAT_HOST` | `127.0.0.1` | 反向 WebSocket 监听 IP |
| `MIO_NAPCAT_PORT` | `6199` | 反向 WebSocket 监听端口 |
| `MIO_NAPCAT_TOKEN` | *(空)* | OneBot 11 访问令牌（Access Token，留空不校验） |

### 4. 日志系统配置

| 环境变量 | 默认值 | 说明 |
|---|---|---|
| `MIO_LOG_LEVEL` | `info` | 日志级别：`debug`、`info`、`warn`、`error` |

---

## 📂 源码目录结构（src/）

```
src/
├── adapters/                     # 平台适配器
│   ├── console/                  # 控制台 REPL 交互实现 (ConsoleAdapter)
│   └── napcat/                   # NapCat OneBot 11 反向 WS 服务端与协议解析
├── config/                       # 配置加载器
│   ├── embedding/                # 向量嵌入端点配置解析 (EmbeddingConfig)
│   └── openai/                   # OpenAI 兼容端点配置解析 (OpenaiConfig)
├── context/                      # 上下文管理与装配
│   ├── achieve/                  # 会话历史冷读取与落盘 (Achieve)
│   ├── contextBuilder/           # 请求上下文装配与水位监控压缩 (ContextBuilder)
│   ├── conversationFusion/       # 会话融合路由与单元 (FusionRouter, FusionContext)
│   ├── costEstimator/            # Token 估算与安全截断 (Tokens)
│   └── summarizor/               # 经历滚动摘要器 (SummaryManager)
├── core/                         # 核心领域实体
│   ├── conversation/             # 会话标识定义 (ConversationKey)
│   ├── event/                    # 领域事件定义 (Event)
│   ├── eventlog/                 # 事件审计日志存储 (EventLog)
│   └── message/                  # 中性统一消息模型 (Msg, Role, ToolCall)
├── diary/                        # 认知日记模块 (Diary)
├── llm/                          # 大语言模型与 Agent 抽象
│   ├── openai/                   # OpenAI 兼容协议客户端与嵌入实现
│   ├── tool/                     # 工具注册表 (ToolRegistry) 与护栏循环 (ToolLoop)
│   ├── Embedding.h               # 向量化抽象接口
│   └── Llm.h                     # LLM 补全抽象契约
├── log/                          # 格式化日志输出 (Log)
├── memory/                       # 长期记忆检索系统
│   ├── manager/                  # 记忆两段式检索编排 (MemoryManager)
│   ├── store/                    # SQLite 原生 BLOB 存储与预过滤 (MemoryStore)
│   └── VectorMath.h              # SIMD 硬件加速向量点积 (AVX-512/AVX2/NEON/SSE2)
├── mind/                         # 潜意识与事实层
│   ├── facts/                    # 稳定 System Prompt 组装 (Facts)
│   └── graph/                    # 人际动态关系图谱 (RelationshipGraph)
├── runtime/                      # 运行时组合根 (Runtime 状态与生命周期调度)
└── main.cpp                      # 应用程序入口 (main)
```
