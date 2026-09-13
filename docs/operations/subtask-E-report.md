# 子任务 E 交付报告：MemoryProvider 接口实现、本地后端适配与不可用测试桩

> 范围：`src/providers/memory/**`（不含冻结的 `MemoryProvider.h`）、`tests/test_memory_provider.cpp`、本报告。
> 实现约束来源：`docs/operations/memory-system-refactor.md`（尤其「子任务 E」「工具和插件接口」「T12」）。
> 本阶段**不做** Hindsight 真实接入（配置/部署/联网/SDK 全部为 `HINDSIGHT_ADAPTER_TODO`）。

---

## 1. 交付状态

| 项 | 状态 |
| --- | --- |
| E1 本地后端适配 `LocalMemoryProvider` | 完成 |
| E2 不可用测试桩 `UnavailableMemoryProvider` | 完成 |
| E3 Hindsight 占位 `HindsightMemoryProvider` | 完成（占位，恒不可用） |
| E4 插件约束（代码注释 + 测试审查断言） | 完成 |
| E5 工厂 `makeMemoryProvider` | 完成 |
| 测试 `tests/test_memory_provider.cpp` | 10 个用例全绿（`mio_tests E_`） |
| Runtime 接线 | 未做（属主 agent；见 §7 接线清单） |
| 共享契约 / xmake / Runtime / config 改动 | 无 |

测试结果（本报告撰写时实测）：

```text
$ ./build/linux/x86_64/release/mio_tests E_
[ DONE ] 运行 10 个用例，检查 7348 次，失败 0 次

$ ./build/linux/x86_64/release/mio_tests        # 全量回归（A/B/C/D/F + 集成）
[ DONE ] 运行 109 个用例，检查 9831 次，失败 0 次

$ flock /tmp/mio_build.lock xmake build MIO      # 主二进制
build ok
```

---

## 2. 文件清单

| 文件 | 说明 |
| --- | --- |
| `src/providers/memory/LocalMemoryProvider.h/.cpp` | 默认后端适配：契约映射 + 三道可见性闸门 + 长度闸门 + 降级 |
| `src/providers/memory/UnavailableMemoryProvider.h/.cpp` | 显式"无长期召回"降级桩（T12） |
| `src/providers/memory/HindsightMemoryProvider.h/.cpp` | Hindsight 占位（恒不可用，错误码 `HINDSIGHT_ADAPTER_TODO`） |
| `src/providers/memory/MemoryProviderFactory.h/.cpp` | backend 名称 → provider 实例（永不返回 nullptr，永不抛） |
| `tests/test_memory_provider.cpp` | E 用例（新建，不修改既有测试） |
| `docs/operations/subtask-E-report.md` | 本报告 |

未改动：`MemoryProvider.h`（冻结接口；本子任务未触碰，其工作区 `M` 状态来自主 agent 的冻结提交）、`src/core/contracts/**`、`xmake.lua`（`src/**.cpp` 通配自动纳入新文件）、`src/runtime/**`、`src/config/**`、`src/main.cpp`、`src/admin/**`、`src/memory/**`、`src/mind/**`、`src/context/**`、`src/providers/llm/**`。`git status` 中除上述归属文件外，E 未修改任何已有文件。

---

## 3. 设计要点

### 3.1 `EpisodeMemory` → `SummaryRecord` 映射规则（`LocalMemoryProvider::toRecord`）

| EpisodeMemory | SummaryRecord | 规则 |
| --- | --- | --- |
| `conversationKey` | `conversationKey` | 直传；为空 → `Rejected`/`INVALID_ARGUMENT`（不推测归属） |
| `text` | `summary` | 直传；为空 → `Rejected`/`INVALID_ARGUMENT`；> `limits::kWriteMaxBytes`(4000) → `Rejected`/`LIMIT_EXCEEDED` |
| `kind` | `kind` | 直传（`EpisodicMemory` / `ContextCompaction` / `Manual` 都允许入库） |
| `visibility` | `visibility` | **只可收窄**：`narrower(episode.visibility, cfg.writeVisibilityCap)`；`MemoryManager` 内部再按它自己的 `writeVisibilityCap` 收窄一次 |
| `fromMessageId`/`toMessageId` | 同名字段 | 必须 `0 < from <= to`，否则 `Rejected`/`INVALID_ARGUMENT`；**绝不**回退游标、**绝不**伪造 ID |
| `createdAt` | `createdAt` | `>0` 直传，否则用当前 epoch seconds（落盘时间不使用 monotonic） |
| `eventTime` | `eventTime` | `>0` 直传，否则回退 `createdAt` |
| `source` | `source` | **直传，不代填**：为空时由 `validateSummaryRecord` 拒绝（溯源字段不得由适配器臆造） |
| `idempotencyKey` | `idempotencyKey` | 直传（`Manual` 记忆的内容幂等键；`MemoryManager` 会在缺省时派生） |
| `participants` | `participants` | 直传；单参与者才归属该人（由 `MemoryManager` 判定 `person_id`） |
| `embedding`/`embeddingStatus` | —— | 忽略：向量化由 `MemoryManager` 在事务提交后执行（失败只置 `failed` + 保留待办） |
| `localId` | —— | `>0` 视为服务端已知记录 → 幂等路径（见下） |

### 3.2 `storeEpisode` 返回四态（绝不撒谎）

| 条件 | status | code | localId |
| --- | --- | --- | --- |
| `localId > 0` 且记录存在、同会话 | `Duplicate` | `Ok` | 原 `localId` |
| `localId > 0` 但记录不存在 / 属其它会话 | `Rejected` | `InvalidArgument` | 0（拒绝伪造/跨会话幂等） |
| 校验失败（空会话/空正文/非法范围） | `Rejected` | `InvalidArgument` | 0 |
| 正文超 4000 字节 | `Rejected` | `LimitExceeded` | 0 |
| `writeSummary` 成功且非重复 | `Stored` | `Ok` | 新记忆 ID |
| 同消息范围/幂等键命中 | `Duplicate` | `Ok` | 原记忆 ID |
| `writeSummary` 报 `InvalidArgument`/`LimitExceeded` | `Rejected` | 原错误码 | 0 |
| `writeSummary` 报存储/冲突等其余失败 | `Failed` | 原错误码（`STORAGE_UNAVAILABLE`/`CONFLICT`） | 0 |
| 依赖抛异常 | `Failed` | `StorageUnavailable` | 0 |

补充：`Stored` 且向量化 `pending`/`failed` 时，`message` 会追加"向量化待处理/失败（文本已保留，待办可重试）"—— 文本确实已确认落盘，但不得包装成"全部完成"。
游标覆盖命中但没有独立记录（`markRangeProcessed` 推进过游标）时返回 `Duplicate` + `localId=0` + 明确说明"未产生新记忆"，而不是编一个 ID。

### 3.3 召回的三道可见性闸门 + 长度闸门

1. **查询前 SQL 预过滤**（`MemoryManager`→`MemoryStore::selectCandidates`）：`visibility` 与会话/归属人条件在 SQL 层裁剪，默认排除 `context_compaction`；
2. **返回前二次复核**（`MemoryManager::isVisibleTo`）：按 `AccessContext` 再判一次，不可见与不存在统一空结果；
3. **适配器返回前复核**（本层，最严）：`query.access.canSee(hit.visibility)` **且** `query.access.canAccessConversation(hit.conversationKey)` 必须同时为真，否则丢弃。当前阶段 `allowCrossConversation` 恒为 false，因此**跨会话一律不返回**（包括 `Public` 记录）——与文档「当前阶段历史查询默认仅限当前物理会话」一致；未来管理员通道打开 `allowCrossConversation`/白名单后，本闸门自动放行，无需改代码。
4. **长度闸门**：单条正文 ≤ `cfg.promptEntryMaxBytes`，累计 ≤ `cfg.promptMaxBytes`（与 `renderForPrompt` 的口径一致）。截断走 `limits::truncateUtf8`（UTF-8 安全），并在文本尾部追加可见标记 `…[已截断]`（标记计入限额，保证"返回字节数 ≤ 限额"可断言）；预算用尽则停止追加并 `log::warn`。
5. **相似度门槛**：取 `max(cfg.minSimilarity, query.minSimilarity)` 中的更严者，防止调用方放宽服务端下限。

### 3.4 降级路径（绝不阻塞主对话）

| 场景 | 行为 |
| --- | --- |
| 后端不可用（`UnavailableMemoryProvider`） | `available()==false`；写入 `Failed`/`STORAGE_UNAVAILABLE`；召回空列表且不抛 |
| Hindsight 未接入 | `available()==false`；写入 `Failed`/`HINDSIGHT_ADAPTER_TODO`；召回空列表 + `lastError` 暴露 TODO 错误码 |
| 未知 backend 名称 | 工厂降级为 `UnavailableMemoryProvider`（记 WARN），**不**回落到本地库 |
| `storeEpisode` 内部异常 | 吞掉 → `Failed`/`STORAGE_UNAVAILABLE`（`localId` 归零） |
| `recall` 内部异常 | 吞掉 → 空列表（降级为无长期召回） |
| 本适配器生命周期 | 无可变状态、无后台线程、无网络连接、无凭据；析构即回收（T12 后半） |

### 3.5 插件约束（E4：代码 + 测试双重体现）

- 本地适配器**只通过 `MemoryManager`** 访问数据，实现文件中不存在文件系统、档案行解析、关系图谱或审批字段的任何入口；
- `LocalMemoryProvider::allowedIncludes()` 声明依赖白名单（`MemoryManager.h` / `MemoryProvider.h` / `Limits.h` / `Log.h`），测试 `E_plugin_sources_touch_no_archive_or_graph_files` 逐行断言 8 个实现文件的引号 include 只落在白名单内，且不含 `Achieve`/`RelationshipGraph`/`relationships.json`/`jsonl`/`ifstream`/`ofstream`/`fstream`/`filesystem`/`opendir`/`fopen`/`getenv` 等标识；
- 测试 `E_plugin_surface_cannot_carry_reasoning` 用**编译期探针**证明 `EpisodeMemory`/`RecallQuery`/`MemoryHit`/`StoreEpisodeResult` 都没有 `reasoning` 字段——即插件接口在类型层面就拿不到 reasoning，并断言召回文本与非正文载荷（`idempotencyKey` 等）无关；
- **该测试是代码审查辅助，不是沙箱**：文本审查挡不住刻意规避的写法，真正的隔离需要进程边界。
- **接口约束不等于进程隔离**：本 C++ 接口不提供沙箱。以同进程方式加载的插件理论上仍能打开宿主文件；若未来需要防不可信插件读取宿主文件，**必须另行实现进程隔离**，不得宣称当前接口已提供隔离。该声明同时写在 `LocalMemoryProvider.h`、`UnavailableMemoryProvider.h`、`HindsightMemoryProvider.h`、`MemoryProviderFactory.h` 的文件头。

---

## 4. 配置示例（backend 选择方式）

E 阶段**不修改 `AppConfig`**（属主 agent）。推荐的接线方式：

```jsonc
// config.json（节选）：在既有 memory 节内新增 backend
"memory": {
  "dim": 1024,
  "maxCandidates": 256,
  "topK": 3,
  "minSimilarity": 0.3,
  "decayTauSeconds": 2592000,
  "writeVisibilityCap": "conversation",
  "maxSummaryBytes": 65536,
  "promptMaxBytes": 12000,
  "promptEntryMaxBytes": 1200,

  "backend": "sqlite-local"   // "sqlite-local"(默认) | "unavailable" | "hindsight"(占位，恒不可用)
}
```

- 缺省/空字符串 = `sqlite-local`；名称比较忽略大小写与首尾空白。
- **未知名称 → `unavailable`**（降级为无长期召回并记 WARN），不抛异常、不回落到本地库：配置写错时宁可"没有长期召回"，也不能把数据写进与配置期望不符的后端。
- 建议环境变量：`MIO_MEMORY_BACKEND`（只覆盖实际存在的变量，语义与 §7 的 JSON 键一致）。
- 代码侧唯一入口：

```cpp
auto provider = mio::makeMemoryProvider(cfg.memoryBackend, memoryManager, cfg.memory);
```

---

## 5. 最小可复现验证步骤

```bash
cd /home/chiiaya/projects/MIO
flock /tmp/mio_build.lock xmake build mio_tests
./build/linux/x86_64/release/mio_tests E_
```

期望输出（0 失败）：

```text
[ RUN  ] E_T12_unavailable_provider_never_claims_success
[ RUN  ] E_T12_factory_selects_backend_and_degrades_safely
[ RUN  ] E_local_roundtrip_store_recall_and_idempotency
[ RUN  ] E_context_compaction_not_recalled_by_default
[ RUN  ] E_visibility_gates_block_cross_conversation_and_public
[ RUN  ] E_recall_respects_length_limits_and_truncates_utf8_safely
[ RUN  ] E_store_failures_never_report_success
[ RUN  ] E_T12_provider_destruction_has_no_threads_or_dangling_state
[ RUN  ] E_plugin_surface_cannot_carry_reasoning
[ RUN  ] E_plugin_sources_touch_no_archive_or_graph_files
[ DONE ] 运行 10 个用例，检查 7348 次，失败 0 次
```

回归（可选）：`./build/linux/x86_64/release/mio_tests`（全量 109 用例，实测 0 失败；E 只新增文件，不改他人代码）。
主二进制：`flock /tmp/mio_build.lock xmake build MIO`（实测 `build ok`，新文件经 `src/**.cpp` 通配自动编译）。
测试全部使用系统临时目录下的 SQLite（用例结束删除）、fake Embedding，**不联网、不使用真实数据/凭据**。

### 验收项对照

| 验收点 | 对应用例 |
| --- | --- |
| T12 插件不可用：`available()==false`、写入 `Failed`/`STORAGE_UNAVAILABLE`（非 Stored/Queued）、召回空且不抛 | `E_T12_unavailable_provider_never_claims_success` |
| T12 工厂：`hindsight`/未知名称同样降级；Hindsight 错误码 `HINDSIGHT_ADAPTER_TODO` | `E_T12_factory_selects_backend_and_degrades_safely` |
| T12 关闭时回收：无线程/无网络，析构即回收，依赖仍可用 | `E_T12_provider_destruction_has_no_threads_or_dangling_state` |
| 本地往返：Stored + `localId>0`、同范围 Duplicate + 同 ID、`localId` 幂等、召回字段映射、topK | `E_local_roundtrip_store_recall_and_idempotency` |
| `context_compaction` 默认不召回、显式允许才召回 | `E_context_compaction_not_recalled_by_default` |
| 可见性过滤：跨会话查不到、Public 受 `maxVisibility` 约束、不泄漏条数/文本 | `E_visibility_gates_block_cross_conversation_and_public` |
| 长度限制：单条/总量限额、UTF-8 安全截断且可观察 | `E_recall_respects_length_limits_and_truncates_utf8_safely` |
| 失败不撒谎：非法范围/空会话/空正文/超长/伪造 ID/存储不可用 | `E_store_failures_never_report_success` |
| 隔离约束：输出不含 reasoning 等非正文载荷 | `E_plugin_surface_cannot_carry_reasoning` |
| E4 插件不得接触档案/图谱/审批 | `E_plugin_sources_touch_no_archive_or_graph_files` |

---

## 6. 对现有接口的兼容性说明

1. **复用既有 `MemoryProvider`，未新增同义接口**：没有引入 `MemoryBackend` 或任何与 `storeEpisode`/`recall` 含义重复的抽象；`MemoryProvider.h` 一字未改，追加语义（四态结果、降级、可见性复核）全部落在实现类与工厂里。
2. **未改 Runtime / Config / main / admin / xmake**：`xmake.lua` 的 `src/**.cpp`、`tests/**.cpp` 通配自动纳入新文件（已实测：`mio_tests` 链接到新符号）。所有既有 target 与文件名不变。
3. **未改持久化格式**：没有新增表、列、索引或迁移；`MemoryManager`/`MemoryStore` 的 schema v2 与事务语义原样使用。E 不写档案、不写图谱、不写审批字段。
4. **既有调用路径不受影响**：新增类只被新测试与未来的 Runtime 接线引用；`MemoryManager` 的 `remember`/旧 `recall` 别名未被触碰。
5. **新增的唯一非虚 API**：`HindsightMemoryProvider::lastError()/adapterTodoCode()` 与 `LocalMemoryProvider::allowedIncludes()`（仅测试/排障用，位于新类上，不扩展冻结接口）。
6. **`MemoryProviderFactory` 为纯新增入口**：返回 `shared_ptr`，永不 `nullptr`、永不抛，不改变任何既有默认行为（默认 backend 就是原来的 SQLite 本地路径）。

---

## 7. 给主 agent 的 Runtime 接线清单

### 7.1 构造与生命周期（组合根）

```cpp
// 1) 先建 store / MemoryManager（既有代码）
MemoryStore store(dbPath);
auto embedding = /* 既有 Embedding */;
MemoryManager memoryManager(cfg.memory, embedding, store);

// 2) 再建 provider（memoryManager 必须比它活得久）
std::shared_ptr<MemoryProvider> memoryProvider =
    makeMemoryProvider(cfg.memoryBackend, memoryManager, cfg.memory);   // 默认 "sqlite-local"

// 3) provider 交给 Runtime/工具层持有；析构顺序：provider → MemoryManager → store
```

- provider 为 `shared_ptr`；无线程、无网络、无凭据，**析构即回收**，不需要 shutdown/join 逻辑。
- `cfg.memory` 建议同时用于 `MemoryManager` 与 `makeMemoryProvider`（topK/门槛/长度预算一致；若两处不一致，召回取更严门槛 + provider 的长度预算生效）。

### 7.2 `AppConfig` 键（主 agent 需自行添加，E 无权改）

- 建议键名：`memory.backend`，字符串，默认 `"sqlite-local"`，取值 `sqlite-local|unavailable|hindsight`（大小写不敏感）。
- 建议环境变量：`MIO_MEMORY_BACKEND`。
- 若不便让 B 的 `MemoryConfig` 承载（`MemoryConfig` 在 `memory/manager/MemoryManager.h`，属 B），可在 `to_json/from_json(AppConfig&)` 里手写 `memory.backend` 字段（缺省写默认值），或另开 `memoryProvider` 节；两种方式对 E 的工厂都无影响。
- 校验建议：类型错误按文档「类型或范围错误时整次配置重载失败」处理；**未知字符串不建议判非法**，直接交给工厂降级为 `unavailable` 并记 WARN（配置面更宽容，行为仍安全）。

### 7.3 `available()==false` 时的 Runtime 降级

- `available()==false`（`unavailable` / `hindsight` / 未知名称）：跳过召回注入（或调用后按空列表处理），走"无长期召回"；**不得**因此阻塞或改写主对话流程。
- 写入工具（`memory_save_episode` 等）必须原样把结果转成 envelope，禁止只回自然语言：

| `StoreEpisodeResult` | 工具返回 |
| --- | --- |
| `Stored` | `{ok:true,data:{memory_id,status:"stored"}}` |
| `Duplicate` | `{ok:true,data:{memory_id,status:"duplicate"}}`（幂等命中，不是新写入） |
| `Rejected` + `INVALID_ARGUMENT`/`LIMIT_EXCEEDED` | `{ok:false,error:{code,message}}`（原码） |
| `Failed` + `STORAGE_UNAVAILABLE`/`CONFLICT`/`HINDSIGHT_ADAPTER_TODO` | `{ok:false,error:{code,message}}`（原码） |

- 召回注入仍建议走既有的 `MemoryManager::renderForPrompt(recalled, access, includeContextCompaction)` 口径（带来源标注、非指令声明），或至少保持"来源+日期+可见性"标注；`MemoryHit` 已做单条/总量截断，二次渲染不会再放大预算。
- 本地后端 `available()` 恒为 `true`；单次写入/召回失败以结果码表达，**不要**据此把整个后端下线（读路径可能仍可用）。

### 7.4 集成注意点（需要主 agent/D 确认的输入契约）

1. **消息范围必填**：`LocalMemoryProvider::storeEpisode` 要求 `0 < fromMessageId <= toMessageId`，不满足即 `Rejected`（不推测、不伪造）。Runtime/D 传入 manual 记忆时必须给出真实证据消息 ID（文档要求 manual 记忆有来源消息引用）。
2. **`source` 必填**：适配器不代填来源；调用方需给 `silence_summary` / `model_tool:memory_save_episode` 等真实来源。
3. **正文 ≤ 4000 字节**（`limits::kWriteMaxBytes`，冻结常量）：超限返回 `Rejected`/`LIMIT_EXCEEDED`（符合文档「超限写入返回 LIMIT_EXCEEDED」）。
   实测当前 Runtime 的摘要器是 `SummaryManager(800, llm)`，正文被 `cutUtf8Safe(..., 800*2+32)` 限制在 **1632 字节**以内，不会触发该上限；只有把 `maxSummaryChars` 提到约 1984 以上才可能触发。届时应先与 E 确认是"上游截断"还是"放宽常量"——这属于契约层面的决定，E 未自行扩大范围。
4. **向量化 pending/failed 不等于失败**：`storeEpisode` 返回 `Stored` 且 `message` 已注明；重试仍走 `MemoryManager::retryPendingEmbeddings`。

---

## 8. 未完成项与数据风险

1. **`HINDSIGHT_ADAPTER_TODO`（四项全未实现）**：Hindsight 的**配置**、**部署**、**联网传输**、**真实 SDK** 均未实现；本阶段只有一个恒不可用的占位实现（`name()=="hindsight"`、`available()==false`、写入 `Failed`/`HINDSIGHT_ADAPTER_TODO`、召回空列表）。**未引入任何新第三方依赖，未写任何真实网络代码，未读写任何凭据。**
2. **没有沙箱**：本 C++ 接口不提供进程隔离。`LocalMemoryProvider` 通过 `MemoryManager` 访问数据、实现文件不含文件系统入口，这是**接口与代码层面的约束**，不是安全边界；同进程加载的不可信插件仍可自行打开宿主文件。需要真隔离时必须另行设计（子进程 + IPC + 能力白名单），并且不得宣称当前接口已提供隔离。
3. **文本审查的局限**：`E_plugin_sources_touch_no_archive_or_graph_files` 是代码审查辅助（include 白名单 + 禁止标识扫描），可被刻意规避的写法绕过；文件不可读时它会跳过并打印 WARN（白名单断言仍执行）。
4. **跨会话 Public 语义被刻意收窄**：第三道闸门要求 `canAccessConversation` 成立，因此 `Public` 记录在别的会话里**不会被召回**（`MemoryManager` 单层复核则会返回）。这是按文档「当前阶段跨会话查询暂不开放」执行的更严策略；未来管理员通道打开 `allowCrossConversation` 后行为自动放宽。
5. **降级桩不回落到本地库**：未知 backend 一律 `unavailable`。若主 agent 期望"未知名称回落到 sqlite-local"，需要显式改工厂（当前行为更安全，故未采用回退）。
6. **数据风险：无写入放大**。E 不新增持久化结构、不删除数据、不迁移旧库；`Rejected`/`Failed` 路径保证零写入（测试断言 `count()==0`）。唯一的数据面影响是"非法/超长输入被拒绝"，可能在 Runtime 未按 §7.4 传参时表现为"模型以为记住了但实际被拒绝"——此时工具层必须返回错误码，这正是 `Rejected` 的用途。
7. **未做的验证**：真实 Hindsight 后端（无实现）、真实 embedding 端点的召回质量（E 用 fake，向量质量属 B/集成测试）、管理员通道的跨会话召回（`adminChannel`/`allowCrossConversation` 当前恒 false，未接入）。

---

## 9. 契约变更请求

无。E 未修改任何冻结契约；如需变更会在本节列出并等待主 agent 决策。当前实现完全落在 `MemoryProvider.h` 已冻结的接口内。

---

## 10. 交付声明（固定格式）

```text
人工审阅依赖：无
是否修改持久化格式：否
迁移是否可回滚：是
权限边界是否改变：否
```

说明：
- **人工审阅依赖：无** —— E 不产生需要人工审批的提案，也不实现任何审批入口；占位符语义未被触碰。
- **是否修改持久化格式：否** —— 无新增表/列/索引/迁移，沿用 `MemoryStore` schema v2。
- **迁移是否可回滚：是** —— 未引入迁移；回滚方式为移除/忽略新代码路径，既有数据与 schema 不受影响。
- **权限边界是否改变：否** —— 未放宽任何权限：只新增了**更严**的返回前复核（跨会话一律不返回、`Public` 需 `canSee` 且会话可达）、长度上限与失败降级；未新增跨会话读取能力，未扩大写入可见性（仍只可收窄）。
