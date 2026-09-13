# 子任务 A 交付报告：配置与会话生命周期（`ConversationLifecycle`）

> 对应文档：`docs/operations/memory-system-refactor.md`（「配置约定」「子任务 A」「配置和调度的精确定义」「必须覆盖的验收用例」）。
> 本报告只描述子任务 A 的实现；Runtime、AppConfig、admin、xmake.lua 由主 agent 负责接线。

---

## 1. 交付文件清单

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `src/context/lifecycle/ConversationLifecycle.h` | 新增 | 静默调度器公开 API（`IClock`/`SystemClock`/`PendingRange`/`DueDecision`/`SummaryRunState`/`ConversationLifecycleState`/`ConversationLifecycle`） |
| `src/context/lifecycle/ConversationLifecycle.cpp` | 新增 | 实现：状态机、投影缓存、冻结、重试、热更新、原子持久化、崩溃恢复 |
| `tests/test_lifecycle.cpp` | 新增 | 16 个用例 / 282 次断言（T01–T04 + 重试耗尽 + 持久化 + 阈值 + 假时钟） |
| `docs/operations/subtask-A-report.md` | 新增 | 本报告 |

**未修改任何既有文件**：`git status` 中 A 的改动仅为上述新增文件（`src/context/lifecycle/` 为新增目录）。`xmake.lua` 的 `src/**.cpp` 通配已自动纳入新文件，无需改动构建脚本。

未触碰：`src/runtime/**`、`src/config/**`、`src/main.cpp`、`src/admin/**`、`xmake.lua`，以及其他子任务目录（`mind/**`、`memory/**`、`context/achieve/**`、`context/summarizor/**`、`context/contextBuilder/**`、`context/conversationFusion/**`、`providers/**`）。

**未修改任何冻结契约**（`core/contracts/**`、`providers/memory/MemoryProvider.h` 全部只读依赖）。

---

## 2. 设计要点（逐条对照硬约束）

### 2.1 pull 模型：没有常驻线程（满足「不为每条消息创建独立常驻线程」）

本类**不创建任何线程 / timer / 后台任务**。调度由 Runtime 单线程周期调用：

```
shouldSummarize(key, now)  →  tryBeginSummary(key, now)  →  [摘要执行者]  →  complete/fail/cancel
```

因此不存在「旧线程晚到」的锁竞态；文档中的「旧定时任务晚到」被建模为**已失效的 taskId**：
只有仍是该会话**当前** `summaryTaskId` 的结果才会被接受（见 2.4）。
`~ConversationLifecycle()` 不需要 join 任何东西；若析构时仍有在飞任务，会打 WARN 提示 Runtime 先回收。

### 2.2 等待条件（`shouldSummarize` / `tryBeginSummary` 双重校验，全部同时满足才到期）

| 条件 | 判定 | 不满足时的稳定 reason |
| --- | --- | --- |
| 静默到期 | `lastActivityAt + silenceTimeoutSeconds <= now` | `silence_not_elapsed` |
| 存在未总结消息 | 档案投影 `count > 0 && from > lastSummarizedMessageId` | `no_pending_messages` / `range_start_not_advanced` |
| 消息阈值 | `minMessagesBeforeSummary == 0` 视为不额外限制；否则 `count >= min` | `below_message_threshold` |
| 无待处理输入 | `inFlightInputs == 0` | `inputs_pending` |
| 无生成请求 | `activeGenerations == 0` | `generation_active` |
| 无在飞摘要 | `summaryTaskId.empty()` | `summary_in_flight` |
| 开关 | `summarizeOnSilence == true` | `summarize_disabled` |
| 未耗尽 | `runState != Exhausted` | `run_exhausted` |

- `tryBeginSummary` 会**原子地**再判一次（从「问」到「起」之间可能已被其他 ingest 线程改变状态），然后一次性冻结范围并捕获 `activityVersion`。
- 「配置值是否正确」不是判定条件之一：`count == 0` 的空范围即使 `minMessagesBeforeSummary == 0` 也不得总结（T04 覆盖）。
- 到期判定**不缓存**：每次查询都用「当前配置 + 当前状态 + 最新投影」重算，因此热更新立即生效（2.7）。

### 2.3 按物理 `ConversationKey` 计时

所有状态以 `ConversationKey::toString()`（`platform + scope + ":" + id`）为键，**不按共享 FusionUnit 计时**。同一 FusionUnit 下多个物理会话各自独立计时、各自一个任务（T01）。

### 2.4 同一会话最多一个有效任务；旧任务即使晚到也不得触发摘要

- `tryBeginSummary` 在 `summaryTaskId` 非空时直接返回 `nullopt`（不会产生第二个任务）。
- 任务 ID 由「冻结时刻 + 进程内序号 + 会话键 + 冻结范围」构成，重试会产生**新的 taskId**。
- `completeSummary` / `failSummary` / `cancelSummary` 先按 taskId 查表，再校验 `st.summaryTaskId == taskId`：
  - 未知/已完成/被取代的 taskId → 记 WARN、返回 `false`、**不改动任何状态**（不推进游标、不增加 retryCount、不清掉当前任务）。
- 冻结的 `activityVersion` 随 `SummaryJob` 下发给执行者，用于「这一轮之后还有新消息」的判定；**它不用于取消已开始的轮次**（否则会违反 T03：总结中收到新消息时冻结范围必须成功提交）。

> **实现说明（对 T02 的解读）**：pull 模型下不存在「每条消息的定时器」，所以 T02 中「旧定时任务晚到（activityVersion 变化后 complete/fail 无效）」被实现并测试为：任务被取代后（失败重试产生新任务、或任务已终态）旧 taskId 的完成/失败回报一律无效。用例 `T02_follower与生成中不提前总结_旧任务晚到无效` 覆盖了该语义，并额外断言「迟到失败不得加计 retryCount」「迟到完成不得推进游标」「当前任务不被清掉」。若主 agent 认为需要「按版本号拒绝回报」的更强语义，需要新增参数（属契约层面的改动，见 §6）。

### 2.5 冻结范围与提交（T03）

- `tryBeginSummary` 冻结 `[from, to]`、`count`、`activityVersion`，并写入 `frozenFromMessageId/frozenToMessageId/frozenCount`。
- `completeSummary(taskId, toMessageId, now)`：
  - `toMessageId <= 0` → 按冻结终点提交；`toMessageId > 冻结终点` → 记 WARN 并**截断到冻结终点**（记录越界尝试）；
  - 游标**只前进**（`max`），绝不越过冻结终点；
  - 提交后**重新投影**：冻结之后到达的新消息保持待总结（`summaryPending`/`hasPendingMessages` 为 true，`pendingFromMessageId` 指向新消息），等待它们自己的静默窗口。

### 2.6 重试与耗尽

- `failSummary` 保留 `summaryPending = true` 与冻结范围，`retryCount += 1`（**不含首次尝试**），游标不动 —— 失败永远不会被当成「已总结」。
- `retryCount > maxPendingSummaryRetries` → `runState = Exhausted`（可观察失败状态），`lastError` 形如：

```
SUMMARY_RETRY_EXHAUSTED: 摘要连续失败 2 次（maxPendingSummaryRetries=1，重试次数不含首次尝试）；需人工排障 HUMAN_REVIEW_REQUIRED；最后一次错误: 第二次失败
```

  - `maxPendingSummaryRetries = 1` → 第二次失败即 Exhausted；`= 0` → 首次失败即 Exhausted（均有断言）。
  - 只写错误码占位，**不写审核人、不写审核时间**；不调用任何审阅接口（`HUMAN_REVIEW_REQUIRED` 在此处只是「需人工排障」的错误码标记）。
- Exhausted 后：即使静默到期、仍有未总结消息、甚至来了新消息，都**不再自动起任务**。
- 归还额度的方式只有两种：**成功**（`retryCount = 0`）或**人工复位** `clearFailure()`（仅供管理端/排障入口使用，MUST NOT 注册为模型工具）。
- `cancelSummary()`（应用关闭/回收）**不消耗**重试额度，任务转为可重试的 `Failed`。

### 2.7 热更新（T04）

`configure(cfg)` 先整体校验（复用冻结契约 `validateConversationLifecycle`），**非法时直接返回 false 并记 WARN，配置与全部会话状态一个字节都不改**；合法时只替换配置值，不动游标、不动重试状态、不取消在飞任务。

由于到期判定不缓存：缩短阈值立即生效、延长阈值立即重算、关闭开关立刻停止新调度（已启动任务仍可完成）、重新开启只处理「未提交范围」（从当前游标重新投影）。

### 2.8 持久化

- JSON 结构（`version` 为 schema 版本，当前 `1`）：
  `{ "version": 1, "savedAt": <epoch>, "conversations": [ {conversationKey, activityVersion, lastActivityAt, lastSummarizedMessageId, pendingFromMessageId, pendingCount, summaryPending, summaryTaskId, retryCount, runState, lastError, lastAttemptAt, hasPendingMessages, frozenFromMessageId, frozenToMessageId, frozenCount} ] }`
- 必落盘项：`retryCount`、`summaryPending`、`lastSummarizedMessageId`、冻结范围、`runState`、`lastError`、`lastActivityAt`（epoch seconds，绝不用 monotonic 值）。
- **不落盘**：`inFlightInputs` / `activeGenerations`（进程内瞬时计数，重启后必然为 0，落盘反而会永久卡住会话）。
- 原子写：同目录 `*.tmp` + `flush` + `rename`；失败时删除 tmp 并保留原文件。
- `load()` 失败（文件不存在 / JSON 非法 / `version` 不匹配）→ 保持当前内存状态不变、**绝不改写原文件**；单条损坏只跳过该条。
- **崩溃恢复**：加载时把 `Running`（含残留 `summaryTaskId`）转成可重试的 `Failed`，`lastError` 记录「上次进程退出前未完成（taskId=…）」，`retryCount` 原样保留 —— 重启既不刷新重试额度，也不会让会话永久停在 Running。

### 2.9 线程安全

单个 `std::mutex` 保护状态表、taskId 索引、配置与投影缓存；`snapshot()/snapshots()/conversations()` 返回**值拷贝**；`persist()` 在锁内做 JSON 快照、**锁外**写文件（不阻塞 ingest 线程的 `note*`）。没有任何跨锁嵌套。

### 2.10 明确不做的事

- **不提供工具内部事件入口**：没有「工具调用/工具结果」上报 API，工具事件无法延长静默时间（用例 `A_待处理与生成上报不延长静默时间` 断言 `noteInputBuffered/noteInputConsumed/noteGenerationStart` 不改 `activityVersion`/`lastActivityAt`，只有 `noteGenerationEnd` 才刷新活动时间）。
- 不伪造参与者、不伪造审核人/审核时间、不调用审阅接口、不写任何 store、不做权限判断（权限属其他子任务）。

---

## 3. 公开 API 与建议签名的差异（语义与字段无缺失）

| 项 | 说明 |
| --- | --- |
| `shouldSummarize/tryBeginSummary/note*/completeSummary/failSummary` | 语义、字段、返回类型与建议一致 |
| `now` 参数 | 全部带默认值 `= -1`；`now <= 0` 表示「使用注入时钟的当前时间」，因此 `noteIncomingActivity(conv)` 等单参调用也合法（建议的直接调用方式不变） |
| `PendingRange` | **追加**可选字段 `std::vector<std::string> participants`（档案投影可顺带给出参与者 internalId）。按位置聚合初始化 `PendingRange{from, to, count}` 仍然合法 |
| `ConversationLifecycleState` | **追加**诊断/落盘字段 `frozenFromMessageId / frozenToMessageId / frozenCount`（追加在末尾，不影响按位置初始化） |
| `SummaryRunState` | 追加 `toString/parseSummaryRunState`（持久化需要），字符串取 `idle/running/failed/exhausted` |
| 新增 `cancelSummary(taskId, reason, now)` | 关闭/回收在飞任务用，不消耗重试额度。**每个已开始的 job 必须恰好回报 complete/fail/cancel 之一** |
| 新增 `clearFailure(conv, now)` | 人工排障复位 Exhausted/Failed（仅管理端使用） |
| `persist()/load()` | 返回 `bool`（建议为 `void`；忽略返回值的老写法仍可编译） |
| `configure()` 非法配置 | 返回 `false` + WARN 日志；`config()` 保持旧值 |

---

## 4. 配置示例（`config.json` 的 `conversation` 节）

```json
{
  "conversation": {
    "silenceTimeoutSeconds": 120,
    "summarizeOnSilence": true,
    "minMessagesBeforeSummary": 2,
    "maxPendingSummaryRetries": 3
  }
}
```

- 校验范围（由冻结契约 `validateConversationLifecycle` 执行，本模块直接复用）：`silenceTimeoutSeconds 1..86400`、`minMessagesBeforeSummary 0..1000`、`maxPendingSummaryRetries 0..10`；`summarizeOnSilence` 必须为布尔。
- 缺省字段使用默认值；类型错误或越界 → 整次配置重载失败，不部分套用。
- 环境变量：`MIO_SILENCE_TIMEOUT_SECONDS` / `MIO_SUMMARIZE_ON_SILENCE` / `MIO_MIN_MESSAGES_BEFORE_SUMMARY` / `MIO_MAX_PENDING_SUMMARY_RETRIES`（只覆盖实际存在的变量）。
- `conversation.silenceTimeoutSeconds` 只负责「一轮对话是否结束」；`inputBuffer.debounceMs` 只负责合并短时连续消息，两者互不影响。
- `config.example.json` 已包含该节（主 agent 提供），无需 A 再改。

---

## 5. 最小可复现验证步骤（确切命令）

```bash
cd /home/chiiaya/projects/MIO

# 1) 构建（flock 避免与其他子 agent 并发写构建目录）
flock /tmp/mio_build.lock xmake build mio_tests

# 2) 只跑 A 的用例（9 个 A_* 用例）
./build/linux/x86_64/release/mio_tests A_

# 3) 只跑 T01–T04（含 A 的 T0x 用例；T08/T11 属其他子任务）
./build/linux/x86_64/release/mio_tests T0

# 4) 全量（其他子任务并行开发时可能有他人用例红/绿，注意区分）
./build/linux/x86_64/release/mio_tests
```

本机实测（交付时）：

- `flock /tmp/mio_build.lock xmake build mio_tests` → `build ok`
- 分过滤器逐个跑 A 的用例（16 个，共 282 次断言，全部 0 失败）：

```text
A_    [ DONE ] 运行 9 个用例，检查 117 次，失败 0 次
T01   [ DONE ] 运行 1 个用例，检查 26 次，失败 0 次
T02   [ DONE ] 运行 1 个用例，检查 47 次，失败 0 次
T03   [ DONE ] 运行 2 个用例，检查 28 次，失败 0 次
T04   [ DONE ] 运行 3 个用例，检查 64 次，失败 0 次
```

- 仅链接 A 的两个 TU + `tests/main.cpp` 的独立二进制 → **运行 16 个用例，检查 282 次，失败 0 次**（与上面一致）
- 全量 `mio_tests`（并行开发中的快照）：**57 个用例 / 1056 次检查 / 失败 2 次**，失败的 2 次均在 `tests/test_archive_coldstart.cpp`（子任务 F 的在改用例），**与本子任务无关**；A 的 16 个用例全绿。
- 并行开发期间曾遇到他人文件导致的构建失败（`FusionContext.cpp`、`Achieve.h`）与改用例导致的他人用例红/绿，按约定等 60 秒重试，未修改他人文件。

手工验证热更新/持久化：见 `tests/test_lifecycle.cpp` 中的 `T04_缩短延长阈值与静默开关`、`A_持久化_*`（全部使用假时钟推进，**不 sleep 真实秒数**、不联网、不使用真实用户数据、库文件写 `std::filesystem::temp_directory_path()` 下的临时目录并在用例结束清理）。

---

## 6. 对现有接口的兼容性说明

- **纯新增**：`src/context/lifecycle/**`、`tests/test_lifecycle.cpp`、本报告。没有改动任何既有头文件/实现/构建脚本，没有新增第三方依赖（只用标准库 + `nlohmann_json`，均已可用）。
- 只读依赖：`ConversationLifecycleConfig`（`core/contracts/LifecycleConfig.h`）、`SummaryJob/SummaryKind`（`core/contracts/SummaryContracts.h`）、`ConversationKey`（`core/conversation/Conversation.h`）、`kSummaryRetryExhausted`/`kHumanReviewRequired`（`core/contracts/Errors.h`）、`log::*`。
- 既有 MemoryManager 的摘要路径**不受影响**：本模块不写任何 store，只产出 `SummaryJob` 并维护自己的调度状态；在 Runtime 接线前它完全不参与生产流程（属于「未完成项」，见 §7）。
- 状态文件是**新增**文件（建议 `data/conversation_lifecycle.json`），与既有档案/图谱/记忆库无耦合；不配置 `stateFile` 时退化为纯内存（不推荐生产使用）。
- **契约变更请求：无**。冻结文件一个字节未改。

---

## 7. 未完成项与数据风险

### 未完成项（不在 A 的独占范围）

1. **Runtime 接线未做**（属主 agent）：在接线前，静默摘要不会被任何生产代码触发（`ConversationLifecycle` 无自驱动线程，这是刻意的）。接线清单见 §8。
2. 摘要执行者（B）尚未消费 `SummaryJob`；`completeSummary/failSummary` 的调用点尚未存在。
3. 管理端 `clearFailure`/`snapshots` 展示未接入（admin 属主 agent）。
4. 人工审阅产品未实现（全局后续占位）；A 只在 `lastError` 中保留 `HUMAN_REVIEW_REQUIRED` 错误码占位。
5. 没有「手工设置初始游标」的维护 API：首次部署的存量历史只能靠投影范围自然消化（见风险 R1）。

### 数据风险

| 编号 | 风险 | 影响 | 建议缓解 |
| --- | --- | --- | --- |
| R1 | **首次部署 / 状态文件丢失时 `lastSummarizedMessageId = 0`**，档案投影会给出「全部历史」范围；静默到期后会请求对整段历史做摘要 | 可能对老会话产生一次很大的摘要请求（token 成本/超时），并写入 B 的经历记忆 | 主 agent 接线时让 provider 对 `from` 设合理下限（如最近 N 条/最近若干天），或先只对「本次启动后收到的消息」投影；接线前不要打开 `summarizeOnSilence` |
| R2 | **双游标**：B 在自己的事务里写摘要游标，A 也维护 `lastSummarizedMessageId`，两者只在 `completeSummary` 调用时对齐；崩溃窗口内可能重复请求同一范围 | 重复摘要（重复经历记忆/token 浪费） | A 侧：每次终态后立即 `persist()`；B 侧必须实现文档要求的「范围幂等 + 提交时比较旧游标」，相同范围重试返回原记录（当前重试在无新消息时 `uniqueKey` 完全一致，已由用例断言） |
| R3 | `stateFile` 未配置或从未 `persist()` | 重启后游标/重试额度丢失 → 等价于 R1 + 重启刷新重试额度 | Runtime 必须配置 `stateFile`，终态后与关闭前 `persist()` |
| R4 | 静默判定用 **system_clock（epoch）**：系统时间被回拨会推迟摘要，前跳会提前触发 | 调度时间偏差 | 文档要求落盘时间必须 epoch，因此这里刻意不用 monotonic；运维避免大幅改系统时间 |
| R5 | 档案投影实现错误（例如忽略 `afterMessageId`） | 可能重复总结已提交范围 | A 已做防御：投影返回 `from <= lastSummarizedMessageId` 时拒绝调度并记 WARN（宁可不总结也不重复总结）；请按 §8(2) 实现 provider |
| R6 | 在飞任务的执行者崩溃/未回报 | 该会话停在 `Running`，不再自动调度 | 进程重启时 `load()` 会自动兜底转 `Failed`；同一进程内必须保证 complete/fail/cancel 恰好一次（§8(5)） |
| R7 | 参与者字段：provider 不给 participants 时任务参与者为空 | 摘要写入缺少参与者元数据（违反「后台工作必须显式携带参与者」） | A 不伪造，只在 begin 时打 WARN；Runtime 必须在投递前补齐（`SummaryJob` 是值拷贝） |

---

## 8. 给主 agent 的 Runtime 接线清单

### (1) 构造与加载（Runtime 初始化阶段，早于任何消息）

```cpp
// 建议用 shared_ptr 持有，保证在飞任务回调不会 use-after-free
lifecycle_ = std::make_shared<ConversationLifecycle>(
    appConfig->conversation,                              // 冻结契约里的 conversation 节
    std::make_shared<SystemClock>(),
    dataDir / "conversation_lifecycle.json");             // 必须配置，否则 R3
lifecycle_->setPendingRangeProvider(
    [this](const std::string& key, std::int64_t after) { return projectPending_(key, after); });
lifecycle_->load();                                       // 启动一次；失败保持默认（不破坏原文件）
```

### (2) `PendingRangeProvider`：用档案投影实现

- 输入：物理会话键、`afterMessageId`（= 已提交游标）。
- 只统计**已确认落盘**且 `messageId > afterMessageId` 的消息；**只计用户消息 + 可见助手消息**，剔除 reasoning、工具调用/工具结果、系统/框架提醒、未落盘消息。
- `fromMessageId` = 范围内第一条可总结消息 ID；`toMessageId` = 最大可见消息 ID；`count` = 命中条数；**没有命中 → 返回 `PendingRange{}`（全 0）**。
- 建议同时填 `participants`（参与者 internalId，不要用昵称）；拿不到就留空，Runtime 在投递前补齐。
- 该回调会在调度 tick 内被调用（每个会话每个版本/游标最多一次，A 有投影缓存），必须是**纯读、有界、无网络**；先取快照后释放锁，不要在回调里做重活或调用模型。

### (3) 驱动线程与周期

- **单线程**调用（可复用现有周期线程），建议 tick = **1 秒**；上限不超过 `max(1, silenceTimeoutSeconds/4)`（120s 静默时 1s 完全够用，不要低于 1s）。
- 每 tick：遍历档案投影里的**物理会话键列表**（也可用 `lifecycle_->conversations()`；首次部署枚举档案时 A 会为新键懒建状态并从「现在」起算一个静默窗口），对每个 key：

```cpp
const DueDecision d = lifecycle_->shouldSummarize(key, now);
if (!d.due) continue;                       // 不要缓存 d，每个 tick 重新问
auto job = lifecycle_->tryBeginSummary(key, now);
if (!job) continue;                          // 期间状态变了，下一 tick 再说
if (job->participants.empty()) job->participants = participantsOf_(key);  // 见 (2)/R7
dispatchToSummarizer(*job);                  // 归属只认 job->conversationKey，禁止读 thread_local
```

- 并发上限：同时在飞的静默任务建议 **≤ 1–2**（避免同时对多个会话打 LLM）；A 只保证「每会话一个」，跨会话限流由 Runtime 做。

### (4) 各 `note*` 的调用点（务必成对，异常路径用 RAII）

| 事件 | 调用 | 位置 |
| --- | --- | --- |
| 收到有效消息 | `noteIncomingActivity(conv)` | 每条已接收消息、进入 InputBuffer **之前**；**包括 follower 路径**（同一 FusionUnit 下第 2 条起的消息也要上报）；群聊按每个参与者的消息分别上报。工具调用/工具结果/框架提醒/reasoning **不要**上报（A 也没有相应入口） |
| 消息被 InputBuffer 接受 | `noteInputBuffered(conv)` | 进入 debounce 等待时 +1 |
| 批被取走送给模型 | `noteInputConsumed(conv)` | 送出时 -1，必须与 buffered 严格配对（异常路径也要 -1） |
| 调用 LLM 前 | `noteGenerationStart(conv)` | 生成期间禁止并发读半成品对话 |
| LLM 返回 | `noteGenerationEnd(conv, now)` | 成功/失败/超时/中断都要调用（RAII guard），否则 `activeGenerations` 永不为 0 → 该会话永久不再摘要 |

`now` 可省略（使用注入时钟）。不要给这两个计数器做「每条消息一对」的嵌套计数。

### (5) 任务终态回报（每个 job 恰好一次）

| 情况 | 调用 |
| --- | --- |
| 摘要写入成功 | `completeSummary(job.jobId, job.toMessageId, now)`——不要传比 `job.toMessageId` 更大的 ID（会被截断并记 WARN） |
| 生成/校验/写入失败 | `failSummary(job.jobId, err, now)`（失败保留 pending 与冻结范围，按额度自动重试） |
| 应用关闭/超时中止/任务被丢弃 | `cancelSummary(job.jobId, reason, now)`（不消耗重试额度） |

投递失败（例如摘要执行者队列已满）也必须 `cancelSummary`，否则会话停在 `Running`（见 R6）。

### (6) 热更新

- `ConfigManager` 发布新 AppConfig 后调用 `lifecycle_->configure(newCfg.conversation)`；返回 `false` 表示候选配置非法 → **保持旧配置**（Runtime 侧同样不应发布该新配置版本）。
- 不要重启 tick 线程、不要重建 lifecycle；已启动任务继续运行（A 不取消它们），未触发的判定下一次查询自动采用新阈值。

### (7) 持久化节奏

- 建议：每次 `complete/fail/cancel` 之后调用一次 `persist()`；再加一个 30–60s 的低频兜底（防止长时间没有终态变更时状态丢失）。
- **关闭前必须 `persist()` 一次**。
- `persist()` 已经是「锁内快照 + 锁外原子写」，可以在调度线程直接调用。

### (8) 关闭回收顺序（对应 T12「应用关闭时任务运行」）

1. 置停止标志并 join 调度线程（不再产生新的 `shouldSummarize/tryBeginSummary`）。
2. 等待在飞摘要任务结束；超时则对每个 job `cancelSummary(jobId, "shutdown", now)`。
3. `lifecycle_->persist()`。
4. 先释放摘要执行者/LLM/store，最后释放 `lifecycle_`（若任务线程持有 `shared_ptr`，确认它们已 join 或不再回调）。
5. A 侧析构不会 join 任何线程，只会对遗留的在飞任务打 WARN（作为接线错误的可见信号）。

### (9) 管理端（主 agent）

- 观测：`snapshots()` 输出每个会话的 `runState/lastError/retryCount/pendingCount/frozen*/lastSummarizedMessageId`；`conversations()` 给出稳定的键列表。
- 排障：`clearFailure(key)` 用于 Exhausted 复位（归还重试额度、保留未提交范围）——**只能挂在管理员通道，MUST NOT 注册为模型工具**，也不需要额外的审批流程。
- `lastError` 里的 `HUMAN_REVIEW_REQUIRED` 只是错误码占位；不要把它写进任何审阅 `actor/timestamp` 字段（A 没有也不创建这些字段）。

### (10) 禁止事项清单

- 不要给工具内部事件上报活动（没有入口，也不要新加）。
- 不要在 `shouldSummarize` 之外缓存「到期」结论。
- 不要在 `summaryTaskId` 非空时绕过 A 直接投递同一会话的第二个摘要。
- 不要让同一个 job 被两个线程同时回报，也不要零回报。
- 不要把 `data/` 下真实运行状态文件纳入测试或提交。

---

## 9. 固定格式声明

```text
人工审阅依赖：无
是否修改持久化格式：是
迁移是否可回滚：是
权限边界是否改变：否
```

- **人工审阅依赖：无** —— 不调用任何审阅接口、不写审阅字段；摘要重试耗尽只用 `SUMMARY_RETRY_EXHAUSTED` + `HUMAN_REVIEW_REQUIRED` 错误码占位表达「需人工排障」。
- **是否修改持久化格式：是（仅新增）** —— 新增独立状态文件 `conversation_lifecycle.json`（`version: 1`）；既有 config/档案/图谱/记忆库格式一律未改。
- **迁移是否可回滚：是** —— 删除该状态文件（或把 `stateFile` 置空）即回到默认无状态；回滚后已总结范围的指针丢失，可能重新触发同一范围的摘要请求，由 B 的范围幂等兜底（见 R2）。
- **权限边界是否改变：否** —— 无工具注册、无读写他人数据、无跨会话访问、不做可见性判断。
