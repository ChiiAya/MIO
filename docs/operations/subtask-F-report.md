# 子任务 F 报告：档案稳定 ID、冷启动与上下文压缩

> 范围：`src/core/message/**`、`src/context/achieve/**`、`src/context/contextBuilder/**`、
> `src/context/conversationFusion/**`、`tests/test_archive_coldstart.cpp`、本报告。
> 未修改：`src/runtime/**`、`src/config/**`、`src/main.cpp`、`src/admin/**`、`xmake.lua`、
> `src/core/contracts/**`、`memory/**`、`mind/**`、`providers/**`、`context/summarizor/**`。
> 契约文件零改动（无契约变更请求）。

---

## 1. 文件清单与设计要点

| 文件 | 变更 | 设计要点 |
| --- | --- | --- |
| `src/core/message/Message.h` / `.cpp` | 改 | `Msg` 新增 `messageId`（0 = 未分配）与 `truncated`（投影标记，不落盘）；`msgToJson` 仅在 `messageId > 0` 时写出，`msgFromJson` 缺省 0（旧记录正常解析）；`reasoningContent` 仍随消息持久化（wire 协议需要） |
| `src/context/achieve/ColdStart.h` / `.cpp` | 新增 | 冷启动契约（`ColdStartOptions` / `ColdStartResult` / `IColdStartSource`）+ 纯函数 `selectColdStart()`：双重预算、时间正序、可见性过滤、UTF-8 截断、可观察丢弃计数。不碰文件、不碰模型 |
| `src/context/achieve/Achieve.h` / `.cpp` | 重写 | 实现 `IArchiveReader` + `IColdStartSource`；稳定 ID、旁路索引、损坏报告、`appendChecked`、`pendingRange`、投影与工具回合完整性、启动枚举 `conversationKeys()` + 旁路注册表 |
| `src/context/contextBuilder/ContextBuilder.h` / `.cpp` | 改 | 注入 `IColdStartSource` 后的冷启动兜底（无 LLM）；`BuildResult` 增加冷启动可观察字段；超水位改走 `context_compaction` 压缩 |
| `src/context/conversationFusion/FusionContext.h` / `.cpp` | 改 | `FusionContext` 增加 `participants` / `coldStartDone`；压缩分块全覆盖 + 覆盖情况可观察；可见性只可收窄；工具协议边缘清理 |
| `src/context/conversationFusion/FusionRouter.h` / `.cpp` | 改 | 建 unit 改用无 LLM 冷启动；`FusionConfig` 增加冷启动预算；两处 `bumpIntimacy` → `noteInteraction`（事件来源去重，非按消息加分） |
| `tests/test_archive_coldstart.cpp` | 新增 | 22 个用例 / 455 次检查（含启动枚举 5 个、`pendingRange` 口径 1 个），全部使用临时目录 + fake Llm |
| `docs/operations/subtask-F-report.md` | 新增 | 本报告 |

### 1.1 稳定消息 ID 与旧档案迁移

* **ID 规则**（`Achieve::loadFileLocked`）：逐行扫描（空行不占位置），
  * 行内 `messageId > 当前最大 ID` → **显式 ID 优先**；
  * 否则 → `max(已分配最大 ID + 1, 行号)`，即**按行位置补**，保证严格递增且唯一；
  * **损坏行也占一个位置**（只保留 ID，不产生消息），因此后续行的 ID 不会塌陷。
* 纯旧档案得到 `1..N`；混排（前 N 行无 ID、后面有 ID）以显式 ID 为准并从其后顺延。
* **旁路索引** `<file>.idx.json`（`version / conversationKey / filePath / fileSize /
  lineCount / nextMessageId / ids / updatedAt`，临时文件 + rename 原子写）。
  ID **始终**由文件内容确定性推导，索引只作为可重建快照（存在且
  `fileSize+lineCount` 一致时跳过重写）。删除索引、损坏索引都不会改变结果。
* **不批量改写原档案**：只在追加新记录时带 `messageId`，旧行原样保留（测试逐行比对）。

### 1.2 路径编码防碰撞 + 旧命名兼容

* 新命名：每个非 `[A-Za-z0-9._-]` 字节 → `%XX`，再追加 `-` + FNV-1a 32 位 8 位十六进制；
  编码前缀超过 96 字节时按转义单元边界截断（`%XY` 不切开），哈希保证唯一。
  因此 `a/b` 与 `a_b`、超长 id、中文/emoji/空格/`..` 都落到**不同文件**。
* 解析顺序 `resolvePathFor`：新命名文件存在 → 用它；否则按旧命名候选
  （`sanitize(toString())`、以及平台前缀引入前的 `sanitize("private:"+id)` /
  `sanitize("group:"+id)`，对应真实的 `private_0d00.jsonl`、`group_10001.jsonl`）
  找到就用它并继续追加；都不存在才用新命名写入。进程内解析结果缓存，不随启动换名。

### 1.3 损坏与写失败可观察

* 解析失败行 → `damageReport()` 记录 `{conversationKey, filePath, lineNumber, reason}` 并
  记 `log::warn`，**继续解析后续行**；整个文件存在但打不开（权限/同名目录）→
  `lineNumber = 0` 的可观察报告，绝不当作"没有历史"。
  注意：`damageReport()` 报告的是**已加载会话**的损坏情况（`Achieve` 惰性加载，且不接收
  会话键），因此调用顺序应为"先读写某会话，再查 damageReport"；损坏在首次加载时确定，
  同一实例内不会重复累积。
* `AppendResult appendChecked(key, msg)`：写 → `flush` → `close` 三次检查，**全部成功后才**
  更新缓存、`lineIds`、`maxId` 并重写索引；失败返回 `ok=false + error`，缓存与最大 ID 不前进。
  兼容包装 `void append(...)` 内部走 `appendChecked`，失败时 `log::error` + 抛
  `std::runtime_error`（void 接口无法回传失败，必须可观察）。

### 1.4 投影不变量（`IArchiveReader`）

* `ArchiveRecord.text` 只含可见正文（多模态消息退化为非 ephemeral 文本 part 拼接；
  图片 URL/base64 不进投影）；`hasReasoning` 只表示"存在但被排除"；
  `toolNames` 只有工具名，绝不带 `argumentsJson`；`isToolMessage` 标记工具结果。
* **工具回合按"成对且闭合"过滤**（选定的方案）：带 `toolCalls` 的助手锚点必须在同一
  投影结果中紧跟**同样数量**的 Tool 结果才算闭合；未闭合的锚点、孤立 Tool 结果整体丢弃。
  因此历史工具调用不会被重放成未完成调用，也不会留下孤立 tool 消息。
* `readRange` 含端点、按 ID 升序、`limit == 0` 表示不限；`from/to <= 0` 视为无下/上界；
  `readRecent` 按文件顺序（= 时间顺序）取尾部并正序返回；`search` 只检索用户/助手可见
  正文（大小写折叠只做 ASCII，中文等非 ASCII 按字节精确匹配 —— 已知局限），
  遵守 `limit`，不返回 reasoning/工具消息/工具参数。
* 所有读取方法在锁内取快照后返回**值拷贝**；`coldStart` 在锁内只做投影，锁释放后才做选择，
  类内不调用任何模型/插件。

### 1.4.1 `pendingRange` 口径（集成追加，已对齐调度器 A）

```cpp
struct Achieve::PendingRange { std::int64_t fromMessageId, toMessageId, count; };
```

* `fromMessageId` = 范围内第一条**可总结**消息；`toMessageId` = 范围内**最后一条可总结
  消息**；`count` = 可总结条数；三者同一口径：只算用户 + 可见助手消息，
  不计 reasoning、工具消息、系统消息、`[框架]` 提醒与旧摘要标记。
* 末尾只有工具 / 纯 reasoning 消息时，`toMessageId` **不会**指向它们；
  没有任何可总结消息时返回 `{0, 0, 0}`（调度器的 `count <= 0 || from <= 0` 判据直接可用）。
* **类型改名（避免编译期冲突）**：调度器 A 在
  `context/lifecycle/ConversationLifecycle.h` 也定义了 `mio::PendingRange`（多一个
  `participants` 字段），而 `Runtime.h` 同时包含两者 → 同名冲突。现把档案层类型改为
  **嵌套类型 `Achieve::PendingRange`**，`mio::PendingRange` 唯一属于调度器。
  消费方写 `Achieve::PendingRange` 或 `auto`，字段逐个拷贝即可（见第 6 节接线项 3）。

### 1.5 冷启动（无 LLM、无长期记忆写入）

* `selectColdStart`：可见性过滤（User/Assistant、非空正文、非 `[框架]` 提醒；Tool 与
  带 toolCalls 的助手整体排除）→ 条数预算 `maxMessages` → token 预算从新到旧累计 →
  反转成时间正序。超长单条：最新一条超预算时按 UTF-8 安全截断并置
  `Msg.truncated = true`（`ColdStartResult.anyTruncated` 可观察），更旧的放不下就停止并把
  剩余条数记进 `droppedByBudget`（不静默）。`rawTokenBudget == 0` 或 `maxMessages == 0`
  返回空（= 不恢复历史原文）。
* `Achieve::coldStart` 只投影尾部窗口（`max(256, 8 × maxMessages)` 条）后释放锁；
  `excludeMessageIds` 排除当前批次，避免重复加入。
* `FusionRouter::createFromColdRead` 改为 `achieve_.coldStart(...)`，**不调用 SummaryManager**，
  冷启动后 `markColdStartDone()`；unit 初始 `topic` 为空、`isPublic = false`（默认私密）。
* `ContextBuilder::build` 兜底：unit 未冷启动时，用
  `remaining = coldStartRawTokens - estimateTokens(systemPrompt) - estimateTokens(当前 unit 上下文)`
  作为历史预算恢复原文，前置到 unit 上下文（当前回合并列其后，不重复），并标记完成。
  预算 ≤ 0 或配置为 0 时直接标记完成、不恢复。
* `Achieve::coldRead(...)`（旧 LLM 冷读取）**已删除**：唯一调用方是 `FusionRouter`
  （本子任务内改完），仓库内已无引用，故不留 deprecated 包装。

### 1.6 上下文压缩（`context_compaction`）

* `FusionUnit::compress` 用 `summary_->summarize(req)`，`req.kind = ContextCompaction`、
  `req.source = "context_compaction"`、`req.visibility = Conversation`（最窄）、
  带 `conversationKey / participants / fromMessageId / toMessageId`；材料是**副本**，
  `reasoningContent` 清空（不碰 unit 内协议数据），`[框架]` 提醒与旧摘要不入材料。
* **全覆盖分块**：目标范围 `[0, n - rawKeep)` 按 `chunkTokens` 切成首尾相接的若干块，
  逐块摘要后**全部**拼进合并摘要（每块带 `[压缩 i/N · 范围]` 标记），块与块之间无空洞。
  `maxChunks` 上限触发时，被丢弃的更早分块**显式**写进正文（"未覆盖"）+ `uncoveredRanges`；
  某块摘要失败同样显式标记并计入 `failedChunks`。彻底移除了旧的"前 20 条 + 末尾 5 条"
  静默丢弃行为。
* **无经历副作用**：只产生摘要文本；不填 `factProposals` / `relationshipProposals`；
  不推进静默游标。`beRedirected` / `reColdStart` 都用同一路径。
* **可见性只可收窄**：建议值经 `narrower(当前, 建议)` 合并，摘要说"公开"不会把私密 unit 变公开。
* 单元上下文里的原文尾部与无 LLM 降级路径都会清理工具协议边缘（未闭合调用/孤立结果），
  避免把未完成调用带进下一次请求。

### 1.7 互动统计（FusionRouter）

群内共同出现与 @ 提及改为调用
`graph_.noteInteraction(a, b, eventSource, now)`，`eventSource = "msg:<unitKey>:<a>-><b>:<now>"`：
同一会话、同一对人、同一秒只计一次互动（图谱侧按来源去重 + 有界），**不按消息条数加分**，
不产生 family/friend 晋级。子任务 C 的 `noteInteraction` 已落地，**没有保留任何兼容层**，
FusionRouter 直接调用。
（`src/context/conversationFusion/FusionRouter.cpp` 两处：群共同出现 415 行、@ 提及 449 行。）

### 1.8 启动枚举 `conversationKeys()`（集成追加）

```cpp
// 已存在档案的会话键（启动驱动静默调度器 / 管理端观测用）
std::vector<std::string> conversationKeys() const;
// 枚举诊断（可观察）：哪些档案没有被枚举、为什么
std::vector<ArchiveDiscoveryDiagnostic> discoveryDiagnostics() const;
```

* **两条来源，输出按 key 排序且稳定**（内部 `std::set`）：
  1. **可重建旁路注册表** `data/history/_conversations.json`
     （`{version, updatedAt, conversations:[{key, file}]}`，临时文件 + rename 原子写；
     `file` 只存文件名，目录整体搬迁不失效）。登记时机：`writeIndexLocked` 中确认档案
     文件存在之后（覆盖"加载已有档案"与"append 成功"两条路径）。读取时按
     `exists(dir/file)` 过滤，文件被删不会列出幽灵会话。
     **丢失可重建**：注册表只是加速器；收到该会话消息 → `resolvePathFor(key)` 命中
     新命名或旧命名文件 → 重新登记。读档案本身从不依赖注册表。
  2. **目录扫描**：只认 `*.jsonl`（显式排除 `*.jsonl.idx.json`、`*.tmp`、注册表本身）。
     新命名做**往返校验**（`decodeKeyStem` 去掉尾部 `-<8位十六进制>` 后百分号解码，
     再 `encodeKey(decoded) == 原文件名` 才采信）；旧命名（`private_0d00.jsonl` 等）
     平台前缀已丢失，**不猜 key**，记一条 `discoveryDiagnostics()` 并跳过。
  3. 另外并入"本进程已解析过的会话"（`resolvedPaths_`，存在性过滤）：注册表因只读目录
     写不进去时仍有兜底。
* **只读**：`conversationKeys()` / `discoveryDiagnostics()` 不创建任何文件、不分配 ID、
  不改写档案；**纯枚举不会创建注册表**（这是明确选择的行为，已用"逐字节目录快照"测试）。
* **异常安全**：单个文件名无法反解、注册表损坏/版本不认识都只记诊断或 `log::warn`，
  绝不让整个枚举失败。
* **已知限制**：① 超长会话键的编码前缀被截断（>96 字节转义），文件名无法无损反解，
  只能靠注册表枚举（扫描时会记诊断）；② 旧命名档案在注册表丢失后无法被枚举
  （平台前缀不可恢复），需要该会话再来一条消息才会重新登记；③ 注册表是"最后写入者获胜"
  的单文件，多个进程同时写同一目录可能互相覆盖（单进程 Runtime 不受影响）。

---

## 2. 配置示例

```json
{
  "contextBuilder": {
    "budgetTokens": 256000,
    "watermark": 0.85,
    "topicWarmupTurns": 5,
    "coldStartRawTokens": 2000,
    "coldStartMaxMessages": 20
  }
}
```

* 两者都是**实施默认值**（不是用户逐项确认的产品需求），且都允许 `0`：
  `coldStartRawTokens = 0` 或 `coldStartMaxMessages = 0` 表示**完全不恢复历史原文**。
* 校验（主 agent 已在 `AppConfig` 完成）：非负、`coldStartRawTokens <= budgetTokens`；
  非法值整次配置重载失败，保持旧配置。
* `FusionRouter` 侧对应 `FusionConfig::coldStartRawTokens / coldStartMaxMessages`（默认同为
  2000 / 20）。`AppConfig` 的 `fusion` 节目前不序列化这两个键，若要让 Router 跟随
  `contextBuilder` 的值，见第 6 节接线项 6。

---

## 3. 最小可复现验证步骤

```bash
cd /home/chiiaya/projects/MIO
flock /tmp/mio_build.lock xmake build mio_tests

# 只跑子任务 F 的用例（22 个）
./build/linux/x86_64/release/mio_tests F_

# 全量回归（含其它子任务用例；当前 D 的用例仍在联调，见下）
./build/linux/x86_64/release/mio_tests
```

最近一次实测结果（release，`-DNDEBUG`，官方 `mio_tests` 二进制）：

```text
[ DONE ] 运行 22 个用例，检查 455 次，失败 0 次      # F_ 过滤（exit=0）
```

**全量运行现状（如实记录）**：全量跑到 D 的 `D_管理员列表权限_T08`
（`tests/test_tools.cpp`，D 正在并行联调）时 **segfault（exit 139）**，另有该用例的
断言失败（`ok=false`、`count=0`）。这与子任务 F 无关：`F_` 过滤运行 exit=0，
且 F 的用例在不依赖他人测试的情况下自洽。等 D 修完后请重跑全量确认。
（另：联调期间 B 的 `SummarySanitizer.cpp` / `MemoryManager.cpp` 曾短暂编译失败，
我用一份只含 F 依赖的临时链接（`/tmp`，未入库）验证过 22 个用例全绿；随后官方
`mio_tests` 也构建成功。）

用例与验收项对照：

| 用例 | 覆盖 |
| --- | --- |
| `F_Msg_messageId持久化与旧记录兼容` | F1：旧记录缺省 0、新字段往返、`truncated` 不落盘、reasoning 仍持久化 |
| `F_稳定消息ID与区间读取` | 1,2,3；`latestMessageId`；`readRange(1,2)=2`；`limit`；`readRecent(2)` 正序；`IArchiveReader` 契约 |
| `F_旧档案迁移_索引可重建且ID稳定` | 旧 jsonl → 1..N；索引删除重建一致；再次加载不变；旧行零改写；追加行带 ID；混排稳定 |
| `F_混排显式ID与按位置补` | 无 ID 行按位置、显式 ID 优先、其后顺延、追加顺延最大值 |
| `F_路径编码防会话键碰撞` | `/`、`_`、`..`、空格、中文、emoji、400 字符超长 id 两两不同文件且实际写入不互相覆盖 |
| `F_损坏行报告位置且后续消息可读` | 第 4 行非法 JSON → `damageReport` 行号+原因；ID 占位；其后消息仍读出 |
| `F_整个文件打不开时可观察` | 权限不可读 → `lineNumber = 0` 的可观察损坏（root 下自动跳过） |
| `F_写失败可观察且缓存不前进` | 同名目录占位 → `appendChecked` ok=false、缓存/latest/pendingRange 不前进、`append` 抛异常 |
| `F_冷启动投影剔除reasoning与工具回合_T06` | T06：reasoning 不入投影（`hasReasoning` 可观察）；孤立/未闭合工具回合剔除；闭合回合成对保留 |
| `F_冷启动双重预算与UTF8截断_T07` | T07：最近 N 条 + 时间正序；0 预算不恢复；超长 UTF-8 截断可观察且不超预算；排除当前批次 |
| `F_ContextBuilder冷启动预算与当前回合优先` | 双预算生效、当前回合最后、不重复恢复、0 预算不恢复、冷启动 0 次 LLM 调用、已落盘当前回合不重复加入 |
| `F_路由冷启动不调摘要LLM_T06` | T06：`FusionRouter::route` 建 unit 用无 LLM 冷启动（fake 计数 0）、默认私密、无 reasoning |
| `F_压缩分块覆盖全部目标范围_T07` | T07：1..45 全部出现在某个分块材料；无 reasoning；尾部原文保留；`maxChunks` 超出显式标记未覆盖 |
| `F_压缩可见性只可收窄_不产生副作用` | 只可收窄；sink 只发 `context_compaction`；不产出事实/关系提案 |
| `F_pendingRange只统计用户与可见助手` | 不计工具/框架提醒/系统/reasoning；只统计已落盘（写失败后计数不变） |
| `F_pendingRange口径对齐最后一条可总结消息` | `toMessageId` 指向最后一条可总结消息；末尾工具/纯 reasoning 不改变它；无候选 → `{0,0,0}` |
| `F_conversationKeys枚举已有会话且排序稳定` | 新会话 append 后可枚举；按 key 排序；重复调用一致；新实例一致 |
| `F_conversationKeys注册表删除后可由目录扫描重建` | 删注册表 → 新实例靠扫描仍列出；纯枚举不重建注册表 |
| `F_conversationKeys纯枚举不改目录字节` | 枚举前后目录逐字节一致；空目录枚举不创建任何文件（含注册表） |
| `F_conversationKeys忽略索引与临时文件并诊断旧命名` | 只认 `.jsonl`；`.idx.json`/`.tmp` 不当档案；旧命名跳过并留诊断；不抛异常 |
| `F_conversationKeys旧命名会话经注册表可枚举` | 旧命名文件收到消息后登记注册表 → 新实例可枚举；注册表丢失后退化为诊断（已文档化） |
| `F_search大小写不敏感且不含reasoning` | ASCII 大小写不敏感、中文命中、reasoning 不检索、工具消息不返回、limit 生效 |

补充人工验证（不写真实 `data/`）：把 `data/history/private_0d00.jsonl`、
`group_10001.jsonl`、`private_888888.jsonl` **复制**到临时目录后加载，得到
`1..18`、`1..62`、`1..264`，`damage = 0`，冷启动分别恢复 10 / 20 / 20 条且
`reasoning = 0`、`toolMsgs = 0`（工具回合整体排除），并在临时目录生成 `<file>.idx.json`。
真实 `data/history/` 目录未新增任何文件。

---

## 4. 对现有接口的兼容性说明

1. **`Msg` 新增字段**
   * `messageId`（0 = 未分配）：`msgToJson` 只在 > 0 时写出，`msgFromJson` 缺省 0 —— 旧 jsonl
     可直接解析，旧代码读到新文件也不会报错（未知字段被忽略）。新消息落盘时由
     `Achieve` 在**确认写入成功后**赋予 ID。
   * `truncated`：纯投影标记，**不落盘**、不上 wire（`toWireMessages` 不受影响）。
   * `reasoningContent` 行为不变：仍随消息持久化并回传 wire（协议需要），只是永不进入
     冷启动投影、历史工具返回与摘要材料。
2. **`Achieve` 新接口**
   * 新增：`appendChecked` / `latestMessageId` / `readRange` / `readRecent` / `search` /
     `damageReport`（实现 `IArchiveReader`）、`coldStart`（实现 `IColdStartSource`）、
     `pendingRange`（返回嵌套类型 `Achieve::PendingRange`）、`conversationKeys` /
     `discoveryDiagnostics`（启动枚举）、静态路径工具（`encodeKey` / `primaryPathFor` /
     `legacyFileNames` / `resolvePathFor` / `decodeKeyStem` / `registryFileName`）。
   * 保留：`load`（原始记录，含 reasoning，仅供迁移/诊断；构建模型上下文必须走投影）、
     `count`、`append(ConversationKey, Msg)` / `append(ConversationKey, vector<Msg>)`。
   * **删除**：`Achieve::coldRead(key, SummaryManager&, int, int)` 与 `struct ColdRead`。
     理由：其唯一用途是"冷启动时同步调摘要模型"，正是 F3 硬约束要移除的路径；仓库内唯一
     调用方 `FusionRouter` 已改为 `coldStart`。C/D/E 若需要"摘要 + 原文"形态的压缩，
     请用 `FusionUnit::reColdStart`（`context_compaction`），不要再引入 LLM 冷启动。
   * **行为变化**：`void append(...)` 失败时由"静默忽略"变为"日志 + 抛
     `std::runtime_error`"。Runtime 若希望降级而非抛出，请改用 `appendChecked`（见第 6 节）。
3. **`ContextBuilder`**
   * `ContextBuilderConfig` / `BuildInput` 字段只增不改，`BuildResult` 新增 4 个可观察字段
     （`coldStarted` / `coldStartRecovered` / `coldStartDropped` / `coldStartTruncated`），
     既有调用方（Runtime）无需改动即可编译。
   * 新增可选依赖 `setColdStartSource(IColdStartSource*)`，不注入时行为与旧版一致
     （只用 unit 上下文）。`build()` 的超水位分支仍是 `unit.reColdStart(*summary_)`，
     只是内部换成 `context_compaction` 分块压缩。
4. **`FusionUnit`**
   * `reColdStart` 返回值从 `void` 变为 `CompressResult`（作为语句调用仍兼容）；
     `beRedirected(SummaryManager&)` 语义不变（返回压缩后的 msgs）。
   * 新增 `coldStartDone()/markColdStartDone()`、`participants()/setParticipants()/mergeParticipants()`、
     `CompressOptions`、`CompressResult` 的覆盖字段。
   * `FusionContext` 聚合初始化仍兼容（新字段带默认值、追加在末尾）。
5. **`FusionRouter`**
   * 建 unit 不再调摘要模型 → **unit 初始 `topic` 为空、`isPublic = false`**（旧行为是
     冷读取摘要给出的 topic 与私密判定）。`canFuse` 对空话题保持既有"预热期不设闸"语义；
     话题此后由 `update_topic` 工具或压缩摘要更新。私密性只会在压缩时被**收窄**，
     不会被放宽。
   * `FusionConfig` 新增 `coldStartRawTokens` / `coldStartMaxMessages`（默认 2000/20）。
   * `bumpIntimacy(…, 0.05, now)`（每条消息加分）→ `noteInteraction(..., eventSource, now)`
     （互动事件去重、有界、不晋级）。
6. **`SummaryManager` 调用方式**
   * 压缩路径调用 `summarize(const SummaryRequest&)`（非 const），不再使用
     `summarizeWithVerdict`；F 侧不修改这两个文件。

---

## 5. 未完成项与数据风险

1. **旧档案索引写盘**：加载旧会话时会写 `<file>.idx.json`（原子写）。索引是纯派生数据，
   删除即可重建；只读目录下写索引失败会被忽略（不影响读取与 ID）。
2. **路径迁移**：旧命名文件不会被改名，新记录继续追加到旧文件；只有全新会话才使用
   新命名。因此一个会话的文件名在生命周期内稳定，但"新旧并存"是预期状态。
   若将来要做统一改名，必须一次性迁移 + 保留回滚副本（本子任务不做）。
3. **损坏文件**：损坏行只报告、不自动修复；损坏行占用一个 ID 位置，因此 ID 会出现空洞
   （这正是"可观察"的体现）。行号是**当前文件**的行号；若人工修复/删行，后续 ID 会变化
   （ID 由内容确定性推导，不承诺跨人工编辑稳定）。
4. **`search` 的大小写折叠只覆盖 ASCII**，非 ASCII 按字节精确匹配（中文可命中，
   拉丁扩展字符不做 Unicode case folding）；检索是 O(n) 全表扫描，靠 `limit` 有界，
   大档案下建议由调用方限制条数。
5. **冷启动窗口**：`Achieve::coldStart` 只投影尾部 `max(256, 8 × maxMessages)` 条记录；
   若尾部几乎全是工具消息，可能恢复不足 `maxMessages` 条可见消息（`droppedToolRounds`
   可观察）。这是有意的有界化，避免大档案全量拷贝。
6. **压缩请求的 `fromMessageId/toMessageId`**：unit 上下文里由 Runtime 追加的当前回合没有
   messageId（0），因此压缩请求的区间是"尽力而为"（只有来自档案恢复的消息带 ID）。
   若需要精确区间，需 Runtime 在 append 时回填 messageId（见接线项 5）。
7. **`dropOrphanToolRecords` 是保守策略**：投影窗口边界上的闭合工具回合可能被整段丢弃
   （宁可少给，不可留下孤立 tool 消息）。
8. **启动枚举的覆盖边界**：`conversationKeys()` 只能枚举"新命名档案"与"注册表登记过的
   旧命名档案"。平台前缀已丢失的旧命名文件（`private_0d00.jsonl` 等）在注册表丢失后
   不会被枚举（只留诊断），需要该会话来一条新消息才会重新登记 —— 即 T05 的
   "重启后游标正确"对旧档案会话依赖注册表（注册表在首次运行后即存在，且可随档案目录
   一起备份；删除它不会破坏档案，只是暂时少枚举到旧命名会话）。
9. **并发多进程**：注册表是单文件、最后写入者获胜；同一目录被多个进程同时写时有丢失登记
   的可能（下一次消息到达会重新登记）。单进程 Runtime 不受影响。
10. **他人文件的 bug（不在我的修改范围，未改）**：`src/context/summarizor/SummaryManager.cpp:105`
    与 `:112` 的 `line.find_first_of("：:")` 会把全角冒号的 UTF-8 首字节（`0xEF`）当成匹配位置，
    `substr(colon + 1)` 于是从 `0xBC` 开始 —— 解析出的 `topic` 以一个残缺字节开头
    （实测 `new_hex=BC9AE5…`）。`privateVerdict` 因只做子串包含判断而侥幸正确。
    影响：`FusionUnit::topic()` 可能是非法 UTF-8，进而影响日志与话题匹配。
    建议 B 改成 `line.find("：")` / 同时处理半角冒号。此为既有行为，非本次引入。

---

## 6. 给主 agent 的 Runtime 接线清单

1. **把 `Achieve` 作为 `IArchiveReader` 交给查询工具（D）**
   ```cpp
   // Runtime 已持有 achieve_；D 的 handler 只依赖接口
   IArchiveReader& archive = achieve_;
   ```
   D 拿到的 `ArchiveRecord` 已剔除 reasoning、工具参数，并已做工具回合完整性过滤；
   消费方**不得**把 `ArchiveRecord` 还原成带 `toolCallId` 的协议消息（投影不提供参数，
   还原会产生孤立 tool 消息）。查询默认限定当前物理会话（`convKey` = `ConversationKey::toString()`）。
2. **冷启动在 `ContextBuilder::build` 中的触发**
   ```cpp
   auto builder = std::make_shared<ContextBuilder>(config->contextBuilder, summary);
   builder->setColdStartSource(&achieve_);   // ← 唯一新增调用（可空依赖）
   ```
   `reloadConfig` 重建 builder 时同样要注入，否则热重载后兜底冷启动失效。
   说明：单次对话的正常路径是 `FusionRouter::route` 建 unit 时已完成冷启动并
   `markColdStartDone()`，`build()` 只对"未标记的 unit"兜底；两条路径都不调 LLM、
   不写长期记忆。`coldStartRawTokens/coldStartMaxMessages = 0` 时两者都不恢复历史原文。
3. **`pendingRange` 喂给调度器（A）—— 口径已对齐，直接逐字段拷贝**
   ```cpp
   lifecycle.setPendingRangeProvider(
       [this](const std::string& key, std::int64_t after) {
           const Achieve::PendingRange r = achieve_.pendingRange(key, after);  // 嵌套类型名
           mio::PendingRange out;   // 调度器自己的类型（ConversationLifecycle.h）
           out.fromMessageId = r.fromMessageId;  // = 首条可总结消息
           out.toMessageId   = r.toMessageId;    // = 最后一条可总结消息（与 A 定义一致）
           out.count         = r.count;
           out.participants  = {};   // 参与者由 Runtime 在投递前补齐
           return out;
       });
   ```
   **语义变更（已按你的要求对齐）**：`toMessageId` 现在是"范围内最后一条**可总结**消息"
   （用户 + 可见助手），不再返回"已落盘最大 ID"。末尾只有工具/纯 reasoning 消息时不会
   指向它们；没有候选时整组返回 `{0,0,0}`，A 的 `count <= 0 || from <= 0` 判据可直接使用。
   **类型名变更**：档案层的类型改名并嵌套为 `Achieve::PendingRange`（用 `auto` 接收也行），
   因为 A 在 `ConversationLifecycle.h` 也定义了 `mio::PendingRange`，`Runtime.h` 同时包含
   两者会重定义冲突（我这边改完已能编译）。`mio::PendingRange` 现在唯一属于调度器。
   该查询只基于 `appendChecked` 成功落盘的消息。
4. **启动时驱动静默调度器（T05 / 重新开启静默摘要）**
   ```cpp
   for (const std::string& key : achieve_.conversationKeys()) {
       // 按 key 恢复/查询游标：pendingRange(key, lastSummarizedMessageId)
       // 只基于已落盘消息；旧命名会话见第 5 节未完成项 8
   }
   // 可观察诊断（旧命名档案没被枚举到等），只记日志，不影响启动
   for (const auto& d : achieve_.discoveryDiagnostics())
       log::warn("Runtime", "档案未纳入启动调度: " + d.fileName + " " + d.reason);
   ```
   `conversationKeys()` 是只读的：不创建文件、不分配 ID、不改写档案，也不重建注册表
   （纯枚举零副作用）；如需刷新注册表，只需让对应会话正常收发一条消息。
5. **压缩路径的 kind / visibility（B、Runtime sink）**
   * F 侧已保证：`req.kind = SummaryKind::ContextCompaction`、`req.source = "context_compaction"`、
     `req.visibility = Visibility::Conversation`（最窄）、可见性只可收窄、不产出
     `factProposals` / `relationshipProposals`、不推进静默游标。
   * **Runtime 侧必须按 kind 过滤 sink**（你已确认负责）：
     `onSummaryProduced` 只处理 `outcome.kind == SummaryKind::EpisodicMemory`，
     否则 `context_compaction` 会被 `memory_.remember(...)` 写成经历记忆。
6. **想消除"未完成项 6"**：Runtime 落档案时改用 `appendChecked` 并把返回的 `messageId`
   回填到送进 unit 的 `Msg`（`unit->append(m)` 前 `m.messageId = r.messageId`），
   这样压缩请求的 `from/to` 区间与 `pendingRange` 都精确。
7. **让 Router 的冷启动预算跟随配置**
   ```cpp
   FusionConfig fusionCfg = config->fusion;
   fusionCfg.coldStartRawTokens = config->contextBuilder.coldStartRawTokens;
   fusionCfg.coldStartMaxMessages = config->contextBuilder.coldStartMaxMessages;
   router_.updateConfig(fusionCfg);   // 或构造时传入
   ```
   （也可让 `AppConfig` 的 `fusion` 节直接序列化这两个键；`AppConfig.cpp` 属你的范围。）
8. **写入失败语义**：`achieve_.append(...)`（Runtime.cpp:669、692）现在失败会抛
   `std::runtime_error`。若要保持"单条落盘失败不打断整轮回复"，请改为 `appendChecked`，
   失败时记日志 + 让该消息不进入 unit 上下文（只有确认落盘的消息才可进入总结范围）。
9. **冷启动行为变化提醒**：新建 unit 的 `topic` 为空、`isPublic=false`；若产品上需要
   "冷启动后立即有话题"，只能由模型工具或后续压缩摘要产生，冷启动不允许调 LLM。

---

```text
人工审阅依赖：无
是否修改持久化格式：是
迁移是否可回滚：是
权限边界是否改变：否
```

* **持久化格式**：jsonl 消息新增可选 `messageId`（旧记录缺省 0，向后兼容；旧读取器忽略未知
  字段）；新增两个**派生的旁路文件**：`<file>.idx.json`（ID 映射快照）与
  `_conversations.json`（会话键 → 档案文件的启动枚举注册表）。二者都不是事实源，
  不影响档案本身的读写。未改动 `data/` 下任何既有文件。
* **可回滚性**：删除 `.idx.json` 与 `_conversations.json` 即可回到"无旁路文件"状态
  （ID 由文件内容确定性重建，注册表可由会话消息重新登记）；不写 `messageId` 的旧版本代码
  仍可读新文件（只是把 ID 当未知字段跳过），旧行从未被改写，回滚不需要数据修复。
* **权限边界**：本次只做档案投影、预算、压缩路径与启动枚举（只读），未放宽任何可见性
  （压缩只可收窄），未新增跨会话读取入口，未改变工具权限。
