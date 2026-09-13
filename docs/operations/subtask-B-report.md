# 子任务 B 交付报告：摘要与经历记忆

> 范围：`src/context/summarizor/**`、`src/memory/**`、`tests/test_summary_store.cpp`。
> 约束来源：`docs/operations/memory-system-refactor.md`（实现约束，非建议）。
> 未修改任何冻结契约头、`src/runtime/**`、`src/config/**`、`xmake.lua` 或其他子任务的文件。

## 0. 完成状态

| 项 | 状态 |
| --- | --- |
| B1 SummaryManager（含全角冒号缺陷修复） | 完成 |
| B2 MemoryStore（schema v2、迁移、单事务、待办、outbox） | 完成 |
| B3 MemoryManager（冻结签名逐字一致） | 完成 |
| `tests/test_summary_store.cpp` | 完成：21 用例 / 316 检查 / 0 失败 |
| 全量回归 `mio_tests` | 97 用例 / 2463 检查 / 0 失败（快照；其他子任务并行推进时数字会变） |

验证命令与结果见第 3 节。阻塞项：无。契约变更请求见第 7 节（均为"可选增强"，不阻塞集成）。

## 1. 文件清单与设计要点

### 1.1 文件清单

| 文件 | 变更 | 说明 |
| --- | --- | --- |
| `src/context/summarizor/SummarySanitizer.h/.cpp` | 新增 | 摘要正文清洗/校验 + UTF-8 合法性 + UTF-8 安全截断（纯函数，可单测） |
| `src/context/summarizor/SummaryManager.h/.cpp` | 重写 | 冻结公开签名不变；按 `SummaryKind` 区分提示词与产出；材料过滤；标记行解析；提案构造；异常兜底 |
| `src/memory/store/MemoryStore.h/.cpp` | 重写 | schema v2、幂等索引、幂等迁移、单事务写入、游标、向量化待办、提案待投递、审计 |
| `src/memory/manager/MemoryManager.h/.cpp` | 重写 | 冻结接口逐字一致 + 3 个追加重载；校验/幂等/可见性收窄/召回双重复核/待办重试/兼容别名 |
| `tests/test_summary_store.cpp` | 新增 | T02/T04/T05/T09 + 范围幂等 + 迁移 + 清洗 + 提案 + 渲染预算 |
| `docs/operations/subtask-B-report.md` | 新增 | 本文件 |

`src/memory/VectorMath.h` 未改动。

### 1.2 事务边界（明确声明）

**同一个 SQLite 事务内**（`BEGIN IMMEDIATE … COMMIT`，`MemoryStore::writeSummaryTx`）：

1. `memories` 摘要行（含 `summary_kind` / `from_message_id` / `to_message_id` / `source` /
   `visibility` / `participants` / `embedding_status='pending'` / `idempotency_key` / `event_time`）；
2. `summary_cursor` 摘要游标（仅 `episodic_memory` 推进，且只允许前进）；
3. `embedding_todo` 向量化待办（每个新写入一行；这是"后端 queued"的唯一本地依据）；
4. `proposal_outbox` 提案待投递记录（摘要同步产出的事实/关系提案）。

事务内任一步失败 → `ROLLBACK`，四者同时不生效；`MemoryManager::writeSummary` 转
`ok=false, code=STORAGE_UNAVAILABLE`，**不得**对模型回"已记住"。

**向量化在事务提交之后**（事务外）：成功 → 同事务更新 `embedding_status='ok'` + 删除待办；
失败 → `embedding_status='failed'` + 待办保留（`attempts+1`、`last_error`）。
**向量化失败绝不回滚已保存的摘要文本**，调用方仍得到 `ok=true`（文本已持久化）+ `embeddingStatus`。

**不参与本事务**：会话档案 JSONL。档案是子任务 F 的已确认落盘数据，摘要在读取范围时已经
是一份值拷贝快照；JSONL 的写入与 SQLite 的写入是两次独立持久化，**不得声称二者构成原子事务**。
因此失败恢复策略是：
* 摘要在 SQLite 中提交成功但进程随后崩溃 → 游标已推进，重启后不会重复摘要（T05）；
* 摘要在 SQLite 中未提交 → 游标未推进，A 的 `failSummary` 保留 `summaryPending` 并重试；
* 档案侧永远只读，不会被摘要流程改写。

`markRangeProcessed`（空范围/无值得保留内容）同样在单事务内：推进游标 + 写 `summary_audit`
审计标记，不产生记忆行；同一范围重复调用幂等（游标不再前进时不再写审计）。

### 1.3 SummaryManager 设计要点

* **全角冒号缺陷修复**：标记行取值不再用 `find_first_of("：:")`（会把 3 字节全角冒号的首字节
  当成匹配位，`substr(colon+1)` 从续字节开始，产出以 `0xBC` 之类续字节开头的非法 UTF-8）。
  现改为先 `find("：")`（按整串匹配，跳过 3 字节）、回退 `find(':')`（跳过 1 字节），并要求
  冒号前恰好是标签，避免把"话题涉及…：…"这类正文行误判为标记行。用例
  `B_topic_marker_fullwidth_colon_utf8` 断言 `topic` 与正文均为合法 UTF-8 且首字节是字符起始字节。
* **材料只用副本、reasoning 一律排除**：`renderMaterial` 对每条消息取 `Msg` 副本并
  `reasoningContent.clear()`、`parts.clear()`；`Role::System` 整条跳过；工具回合只保留
  "发生过工具回合" + **工具名**（`ToolCall::name`），`argumentsJson` 与工具结果正文都不进入材料。
  调用方即便忘了过滤，这里也会再过滤一次。响应里的 `ChatResponse::reasoning` 直接被忽略。
* **按 kind 区分**：`EpisodicMemory` = 经历摘要 + 私密性 + 话题 + 事实提案 + 关系提案；
  `ContextCompaction` = 压缩摘要，**不产出任何提案**（即便模型输出提案行也剥离），不提升可见性；
  `Manual` = 从简提示词，不产出提案。
* **输出解析**：整份输出逐行分类（不再只扫尾部）：`话题：`/`私密性判断：`/`事实提案：`/`关系提案：`
  为标记行（从正文剥离），其余为正文。私密性判定**默认私密**：模型未输出或无法解析时保持最窄，
  只有明确解析出"公开"才置 `privateVerdict=false`（与 `SummaryOutcome` 注释"解析失败保持默认私密"一致）。
* **提案只能进 proposal 层**：模型只提供 `predicate`/`object`/`confidence`
  （confidence 解析失败/越界 → 0.0，钳制到 `[0,1]`）；`subjectId`（取服务端参与者）、
  `conversationKey`、`source`、`evidenceMessageIds`（`conv#from` / `conv#to`）全部由服务端填。
  固定 `status=Pending`、`factStatus=Proposed`、`review.status=HUMAN_REVIEW_PENDING`、
  `review.actor=""`、`review.reviewedAt=0`、`approvedBy=""`、`visibility=Conversation`。
  模型夹带的 `actor=` / `reviewedAt=` / `approvedBy=` / `reviewer` 等字段被**丢弃字段**（不是写入）。
  参与者为空（服务端无法解析身份）时**不产出提案**，绝不冒填 subjectId。
* **正文校验**：`SummarySanitizer` 按行拦截并删除
  ①`HUMAN_REVIEW_*` 占位字段、`reviewedAt`/`approvedBy` 字段名；②reasoning 回显
  （`reasoning_content`/`reasoningContent`/`思维链`/`推理过程`/`chain of thought`）；
  ③`api key`/`apikey`/`api_key`/`Authorization`/`Bearer `/`access token`/`secret key` 字样（整行删除）、
  行内 `sk-xxxxxxxx` 密钥本体（只剔除密钥，行内其余内容保留；整行只剩密钥则删行）；
  ④调用方注册的系统提示特征句（摘要器自己把 prompt 特征句注册进去）。
  能安全删除就删除并记录命中规则；**全部内容行都被判为污染时 `ok=false` + error**，
  绝不静默写空摘要。提案的 predicate/object 也走同一清洗器。
* **失败语义**：`llm_` 为空、调用抛异常、空回复、清洗失败 → `ok=false` + `error`，
  异常绝不穿透到调度线程；`onSummary_` sink 只在公开入口（`summarize` /
  `summarizeWithVerdict`）发一次，`generate`/`generateFor` 内不发。

### 1.4 MemoryStore schema v2

`PRAGMA user_version = 2`（`MemoryStore::kSchemaVersion`）。

`memories` 新增列：`summary_kind`、`from_message_id`、`to_message_id`、`source`、`visibility`、
`participants`(JSON 文本)、`embedding_status`、`idempotency_key`、`event_time`、
`evidence_ids`(JSON 文本)、`migration_note`（旧列 `is_public` 保留但**不再参与任何权限判断**，仅作迁移证据）。

新增表：`summary_cursor(conv_key PK, last_message_id, updated_at, schema_version)`、
`embedding_todo(id PK, memory_id UNIQUE, created_at, attempts, last_error)`、
`proposal_outbox(id PK, memory_id, payload, created_at, delivered, delivered_at, last_error)`、
`migration_audit`、`summary_audit`。

幂等索引（**部分唯一索引**，避开旧行与 manual）：

```sql
CREATE UNIQUE INDEX idx_memories_range ON memories(conv_key, from_message_id, to_message_id, summary_kind)
  WHERE from_message_id > 0 AND summary_kind <> 'manual';
CREATE UNIQUE INDEX idx_memories_idem  ON memories(idempotency_key) WHERE idempotency_key <> '';
```

旧 `UNIQUE(conv_key, summary)` **已不作为幂等依据**：迁移时重建表并去掉该约束，因此
"同名文本 + 不同消息范围"会正常写成两条记录（用例 `B_range_idempotency_not_by_summary_text`）。

**迁移（幂等、可重试、单事务）**：
* 判定"旧库"依据是列是否存在（`PRAGMA table_info(memories)` 无 `summary_kind`），
  而不是只看 `user_version`；每一步都可重复执行（建表用 `IF NOT EXISTS`、重建用事务保护）。
* 旧库处理：`ALTER TABLE memories RENAME TO memories_legacy_v1` → 按 v2 建表 →
  整表搬运（**id 不变**）→ `DROP TABLE memories_legacy_v1`，全部在一个事务内。
  搬运时：`summary_kind='episodic_memory'`、`from/to=0`、`source='legacy_import'`、
  **`visibility='conversation'`**、`embedding_status` 按向量是否完整判定（完整 → `ok`，
  否则 `pending` 并补 `embedding_todo`）、`event_time=created_at`、
  `migration_note='legacy is_public narrowed'`（`is_public=1` 行）。
* 旧行**不删除、正文不丢**：`is_public` 原值保留（`SELECT is_public` 仍为 1），行数不变；
  迁移事实写入 `migration_note` + `migration_audit(action='migrate_v1_to_v2')`。
* 失败（例如残留表 `memories_legacy_v1`）→ 抛错并 `ROLLBACK`，原库保持原样，可人工处理后重试。

### 1.5 MemoryManager 设计要点

* **写入**：`validateSummaryRecord` → 正文上限 → 可见性 `narrower(record.visibility, cfg.writeVisibilityCap)`
  （`context_compaction` 额外强制最窄到 `conversation`）→ 单事务 → 再向量化。
  校验失败 `ok=false, code=INVALID_ARGUMENT` 且不落盘。
* **幂等**：`manual` 用 `idempotency_key`（调用方未给时用 `manual:<fnv1a(conv+person+text)>` 派生）；
  其余用 `(conv, from, to, kind)` 唯一索引；episodic 提交时比较旧游标，`to <= committedCursor`
  视为已提交（duplicate，返回贡献游标的最近记录 ID，可能为 0）。重复提交返回 `ok=true, duplicate=true`
  且 `memoryId` 为原记录 ID，并把原记录的 `embeddingStatus` 回传。
* **游标**：只有 `episodic_memory` 推进；`manual` / `context_compaction` 不推进（用例覆盖）。
* **召回**：SQL 层可见性预过滤（conversation 仅限可访问会话 / person 需 `maxVisibility>=Person`
  且 person_id 匹配 / public 需 `maxVisibility>=Public`；默认排除 `context_compaction`；
  只取 `embedding_status='ok'` 且向量长度匹配的行）→ 内存点积 + 事件时间衰减
  → **返回前二次复核** `isVisibleTo`（不可见与不存在统一空结果，不泄漏隐藏条数/片段）。
  另提供 `renderForPrompt(recalled, access)` 重载做**注入前第三次复核**。
* **降级**：embedding 为空/异常/维度不符/冷却中 → `recall` 返回空，不阻塞主链路；
  连续失败进入 60s 冷却；`retryPendingEmbeddings` 在冷却/后端缺失时直接返回 0（不消耗待办额度）。
* **兼容别名**：`remember(...)` 走同一校验层（kind=manual、4000 字节 UTF-8 截断、可见性只可收窄、
  内容幂等键、不推进游标）；`recall(query, viewerPersonId, convKey, now, topK)` 只构造
  **最窄** `AccessContext`（`maxVisibility=Conversation`、仅当前会话）后走新路径，不绕过权限层。
* **提案投递**：`writeSummary(record, proposals)` 把净化后的提案写入同一事务的 `proposal_outbox`；
  `deliverPendingProposals(IProposalStore&, limit)` 提交给 C 的 `ProposalStore`，submit 成功才标记
  `delivered`（至少一次语义）；失败记录 `last_error` 并保留待投递；损坏 payload 隔离（标记 delivered +
  错误原因），避免永久阻塞队列。

## 2. 配置示例（`memory` 节）

`config.json` 的 `memory` 节（前 5 个键是既有字段，后 4 个是 B 新增的保守默认值）：

```json
"memory": {
    "dim": 1024,
    "maxCandidates": 256,
    "topK": 3,
    "minSimilarity": 0.3,
    "decayTauSeconds": 2592000,
    "writeVisibilityCap": "conversation",
    "maxSummaryBytes": 65536,
    "promptMaxBytes": 12000,
    "promptEntryMaxBytes": 1200
}
```

含义与约束：

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `writeVisibilityCap` | `conversation` | **写入可见性上限**：服务端只可收窄。默认最窄，模型/摘要永远无法把记忆写成 public；只有显式调高（系统/人工策略）才可能落成 `person`/`public` |
| `maxSummaryBytes` | 65536 | 入库正文上限（与 `validateSummaryRecord` 的 64KB 一致）；超过返回 `LIMIT_EXCEEDED` 且不落盘 |
| `promptMaxBytes` | 12000 | `renderForPrompt` 注入块总字节上限（= `limits::kReadMaxBytes`） |
| `promptEntryMaxBytes` | 1200 | 单条召回摘要进入 prompt 的字节上限 |

注意：`AppConfig` 目前只反序列化原有 5 个键（`src/config/AppConfig.cpp` 由主 agent 维护），
新增键即使写进 JSON 也会被忽略并保持上表默认值——默认值即最保守值，功能不受影响；
若要支持从配置文件调整，需要主 agent 在 `AppConfig` 的 `memory` 节补 4 个键（第 7 节变更请求）。

## 3. 最小可复现验证步骤

```bash
cd /home/chiiaya/projects/MIO

# 1) 构建（与其他子任务共用构建锁）
flock /tmp/mio_build.lock xmake build mio_tests

# 2) 本子任务用例（21 用例 / 316 检查）
./build/linux/x86_64/release/mio_tests B_
#   -> [ DONE ] 运行 21 个用例，检查 316 次，失败 0 次

# 3) 全量回归（含 A/C/D/F 用例；本次快照 97 用例 / 2463 检查）
./build/linux/x86_64/release/mio_tests
#   -> [ DONE ] 运行 97 个用例，检查 2463 次，失败 0 次
```

用例 → 验收项对照：

| 用例 | 覆盖 |
| --- | --- |
| `B_T05_cursor_survives_restart_and_range_is_idempotent` | 写入后崩溃重建：游标正确、同范围不重复写、重叠范围 duplicate |
| `B_T05_embedding_failure_keeps_text_and_retries` | 向量失败 → 文本可读 + failed + todo(attempts/last_error) → 重试置 ok |
| `B_T05_missing_backend_keeps_pending_todo` | 无向量后端 → pending + todo（queued 的本地依据） |
| `B_T05_model_timeout_does_not_advance_cursor` | 模型超时 → 不写库、游标不动；重试成功才推进 |
| `B_range_idempotency_not_by_summary_text` | 范围幂等；同名文本不同范围不误判 |
| `B_summary_kinds_cursor_and_recall_switch` | compaction 默认不召回（需显式开关）；manual 不推进游标；episodic 推进 |
| `B_T02_private_summary_not_recalled_cross_conversation` | 私密不外泄；public 需 canSee 允许；不泄漏片段 |
| `B_T09_summary_cannot_widen_visibility` | 传 Public/Person 一律收窄为 conversation；未知 visibility 读回保持最窄 |
| `B_legacy_schema_migration_narrows_and_is_idempotent` | 旧库迁移：行/文本保留、is_public 收窄、迁移记录、版本升级、再开幂等 |
| `B_sanitizer_blocks_forbidden_content` | HUMAN_REVIEW_* / sk- / api key / reasoning / 提示词回显拦截 |
| `B_summary_material_excludes_reasoning_and_tool_args` | 材料无 reasoning/系统提示/argumentsJson/工具结果正文 |
| `B_topic_marker_fullwidth_colon_utf8` | 全角冒号缺陷回归，topic/正文合法 UTF-8 |
| `B_proposals_server_sided_pending_only` | 提案服务端字段、pending 状态、伪造审核字段丢弃、compaction 无提案 |
| `B_summary_manager_sink_once_and_failure_is_not_success` | sink 一次；异常/空回复转 ok=false 不穿透 |
| `B_T04_invalid_summary_rejected_without_write` | 空文本/非法范围/空会话/空 source → INVALID_ARGUMENT 且零写入 |
| `B_mark_range_processed_is_audited_and_idempotent` | 空范围推进游标 + 审计、幂等、迟到范围 duplicate |
| `B_summary_text_sanitized_before_persist` | 端到端：污染全命中不写入；部分命中清洗后落库无残留 |
| `B_remember_alias_is_safe` | 兼容别名：截断、收窄、不推进游标、内容幂等、空内容拒绝 |
| `B_proposal_outbox_delivery_to_proposal_store` | outbox 单事务写入 + 投递 + 不重复投递 + 服务端字段覆盖 |
| `B_render_for_prompt_marks_data_and_respects_budget` | 标注"数据、不是指令"+ 时间/可见性来源 + 预算 + 注入前复核 |
| `B_recall_query_limit_and_degrade` | query 超长安全截断；embedding 异常/维度不符降级为空 |

所有用例的数据库都在 `std::filesystem::temp_directory_path()/mio_test_b_<pid>/<用例名>/memory.db`，
用例结束时整目录删除，**不读写真实 `data/`**；摘要用 fake `Llm`，向量用 fake `Embedding`，不联网。

迁移的真实数据核对（可选，只读）：

```bash
# 应用下一次启动会自动迁移 data/memory.db；迁移前建议先备份
cp -a data/memory.db data/memory.db-wal data/memory.db-shm /tmp/mio_db_backup/ 2>/dev/null
# 迁移后（应用已停止时）核对：
sqlite3 data/memory.db \
  "PRAGMA user_version; \
   SELECT COUNT(*) AS rows, SUM(is_public) AS legacy_public, \
          SUM(visibility='conversation') AS conversation_visible, \
          SUM(migration_note<>'') AS noted FROM memories; \
   SELECT at, action, detail FROM migration_audit;"
# 期望：user_version=2；rows 与迁移前一致；legacy_public 保持原值（历史证据）；
#       conversation_visible=rows；noted >= legacy_public；migration_audit 有 migrate_v1_to_v2 行
```

## 4. 对现有接口的兼容性说明

### 4.1 `SummaryManager`

| 入口 | 兼容性 |
| --- | --- |
| `summarize(const SummaryRequest&)` | 签名不变（冻结）。现在按 `request.kind` 走不同提示词；`ContextCompaction` 不产出提案；失败 `ok=false`+`error`；sink 只发一次 |
| `summarizeWithVerdict(const std::vector<Msg>&) const` | 签名不变（冻结）。语义固定为 `kind=context_compaction`（等价旧"每 N 轮摘要"），无提案 |
| `setOnSummary` / sink | 语义不变：公开入口发一次，`generate` 内不发。注意 Runtime 已不再注册 sink（无重复写入） |
| `generate(const std::vector<Msg>&) const` | 私有签名保留（冻结），内部委托到 `generateFor` |
| 构造函数 `(int maxSummaryChars, std::shared_ptr<Llm>)` | 不变 |

行为差异（有意，方向为"更严"）：标记行缺失时 `privateVerdict` 由 false 变为 **true（私密）**；
工具回合不再把 `argumentsJson` 与结果正文写进材料；正文可能因清洗少掉污染行。

### 4.2 `MemoryManager`

* 冻结签名逐字一致：`writeSummary` / `getSummary` / `listSummaries` / `committedCursor` /
  `markRangeProcessed` / `retryPendingEmbeddings` / `recall(query, access, topK, includeCompaction)` /
  `count` / `renderForPrompt(recalled)` / `remember(...)`（返回类型由 `void` 变为 `SummaryWriteResult`，
  忽略返回值的旧调用点仍可编译，但**新代码必须检查 `ok`**，不得再假定"调用即记住"）。
* 追加重载（不改变上述签名）：
  * `writeSummary(const SummaryRecord&, const std::vector<Proposal>&)` —— 提案同事务入 outbox；
  * `recall(query, viewerPersonId, convKey, now, topK)` —— 旧签名兼容别名，内部构造最窄
    `AccessContext`（`maxVisibility=Conversation`、仅当前会话），走同一过滤与复核路径；
  * `renderForPrompt(recalled, access, includeCompaction)` —— 注入前复核。
* **行为差异（权限收窄，必须知悉）**：
  1. 旧 `recall(viewer, convKey)` 语义是"公开 ∪ 本人 ∪ 本会话"。兼容别名现在只返回
     **该会话的 conversation 可见记录**（不会返回 public/person 记录）——这是"只可收窄"的必然结果；
  2. `remember(..., isPublic=true)` 不再写出 public 记忆：`cfg.writeVisibilityCap` 默认 `conversation`，
     请求被收窄；要恢复旧行为必须由服务端显式调高上限（不建议，且违反 T09）；
  3. `remember` 超长正文按 4000 字节 UTF-8 安全截断（旧实现整段写入）；
  4. `RecalledMemory` 字段按冻结契约变化（`visibility`/`kind`/`from`/`to`/`source` 取代 `isPublic`）；
     Runtime/D 的旧字段访问会在编译期暴露。
* `MemoryConfig` 新增 4 个字段，均有保守默认值；`AppConfig` 现有序列化代码不受影响（只读写原 5 键）。

### 4.3 `MemoryStore`

* 构造/析构/`count()` 语义不变；`insert(MemoryRecord, vec, dim)` 旧接口**已移除**，
  改为 `writeSummaryTx`（旧调用点只有旧版 `MemoryManager`，已同步重写）。
* 旧 schema（`user_version` 0/1、无 `summary_kind` 列）自动迁移到 v2；迁移幂等、事务保护、失败不留半成品。
* 旧 `UNIQUE(conv_key, summary)` 被移除（旧表被重建）。降级风险：若把新库交回旧二进制运行，
  旧代码的 `INSERT OR IGNORE` 不再按文本去重（会插入重复摘要行），而旧代码的 `SELECT` 仍可读新表
  （多余列被忽略）。生产上升级后不保留旧二进制回退路径。

## 5. 未完成项与数据风险

**未完成项（有意不做 / 需其他子任务完成）**

1. 没有管理端/CLI 界面查看 `proposal_outbox` 待投递与 `embedding_todo` 待办；只提供只读访问器与日志。
   排障入口是 `MemoryManager::pendingProposalCount()` / `MemoryStore::pendingEmbeddingTodos()` /
   `summaryAudit()` / `migrationAudit()`。
2. 提案投递是"至少一次"：`submit` 成功后才标记 delivered，但 `submit` 与标记之间崩溃会重复投递。
   C 的 `ProposalStore::submit` 会忽略调用方提供的 `proposalId`（自己生成 `prop-N`），
   因此客户端无法用确定性 ID 去重 —— 见第 7 节变更请求（或接受重复 pending 提案，人工审阅时合并）。
3. 没有实现"`context_compaction` 显式转换为 `episodic_memory`"的接口（文档说只有显式转换才可召回）。
   当前 `includeContextCompaction=true` 是查询开关（D 的读取工具用），转换动作应属于管理端/未来策略层。
4. 未实现摘要的增量/去重合并（同一话题跨范围摘要不合并）；范围幂等已保证不重复，但语义重叠的范围
   仍可能各写一条（`to > cursor` 时）。A 的冻结范围机制正常情况下不会产生这种提交。

**数据风险**

1. **真实 `data/memory.db` 存在**（118KB，另有 `-wal/-shm`）。应用下一次启动会把 schema 从 v1 升到 v2：
   迁移是单事务、失败自动回滚、旧行与正文不丢，但**旧 `is_public=1` 行会收窄为 conversation 可见**，
   即"其他会话可召回"这条能力对旧数据永久消失（这是文档强制要求）。建议升级前备份
   `data/memory.db*`。
2. **迁移成功后不可自动回退**：v1→v2 重建了表结构，回退到旧二进制不会恢复 `UNIQUE(conv_key, summary)`。
   需要回退时用备份文件恢复。
3. **权限边界收紧**（旧 public 记忆、旧 recall 语义）可能让升级后"记忆变少"：属于预期安全行为，
   但需主 agent 在集成时确认（见四行声明）。
4. **身份归属近似**：`SummaryRecord` 没有 owner 字段，`personId` 取"唯一参与者"，群聊（多参与者）
   记为 `""`。因此 `person` 可见性的记忆只可能在单参与者会话中产生；群聊摘要永不归属个人
   （宁可不归属，也不冒填身份）。若需要精确归属，见第 7 节变更请求。
5. **`AppConfig` 未接线新配置键**：无法通过配置文件调整写入可见性上限；默认值即最保守值。
6. **旧档案 JSONL 不参与事务**：SQLite 提交成功而档案写入失败（或反之）都不会损坏记忆库，
   但两者不是同一原子单元；摘要只读档案，不会回写档案。
7. **`markRangeProcessed` 是单方面的**：若 Runtime 在"模型返回空正文"时误判为"无可保留内容"，
   该范围会被永久跳过（仅留审计）。建议只对"档案无可见消息"或"模型明确判定无需保留"调用它。

## 6. 给主 agent 的 Runtime 接线清单

当前 `Runtime::runSummaryJob`（主 agent 已实现）与我的冻结接口已经对接成功，
下面是完整的推荐链路以及两处**建议补强**（`★`）。

1. **静默摘要（A → F → B）**
   1. `ConversationLifecycle::shouldSummarize(key, now)` → `tryBeginSummary(key, now)` 得到 `SummaryJob`；
      校验 `job.valid()` 与 `job.kind == SummaryKind::EpisodicMemory`。
   2. `Achieve::readRange(job.conversationKey, job.fromMessageId, job.toMessageId, 0)`
      → `vector<ArchiveRecord>`（已剔除 reasoning / 工具参数）。
   3. `ArchiveRecord` → `Msg`（role/text/createdAt/messageId/sender*），`reasoningContent` 保持为空；
      构造 `SummaryRequest{kind=job.kind, conversationKey, participants, from, to,
      visibility=job.visibility, source=job.source, now=<epoch seconds>, material}`。
   4. `SummaryManager::summarize(req)`：
      * `!out.ok` → `lifecycle.failSummary(job.jobId, out.error, now)`（保留 `summaryPending` 与重试额度；
        重试耗尽由 A 置 `Exhausted` + `SUMMARY_RETRY_EXHAUSTED` + `HUMAN_REVIEW_REQUIRED` 占位）。
      * `out.ok && out.text.empty()`（理论上不会出现）→ 按"无可保留内容"处理。
   5. **空范围 / 无可保留内容**（`records.empty()` 或明确判定无内容）：
      `MemoryManager::markRangeProcessed(conv, job.toMessageId, "empty_range"/"no_retainable_content", now)`
      → `lifecycle.completeSummary(job.jobId, job.toMessageId, now)`。**不要**走 failSummary（否则无限重试）。
   6. 构造 `SummaryRecord`：`conversationKey/participants/kind=EpisodicMemory/summary=out.text/
      createdAt=now/eventTime=records.back().createdAt/from=job.from/to=job.to/source=job.source/
      evidenceMessageIds={conv#from, conv#to}`；`visibility` 建议
      `out.privateVerdict ? Conversation : narrower(job.visibility, cfg.writeVisibilityCap)`
      （MemoryManager 内部还会再收窄一次，纯粹是双保险；`privateVerdict` 只能用于收窄，禁止放宽）。
   7. **写入**：`writeSummary(rec, proposals)`，其中 `proposals` = `out.factProposals` +
      `out.relationshipProposals`（★建议）：这样提案待投递记录与摘要/游标/待办在同一 SQLite 事务内，
      崩溃不丢提案。当前 Runtime 是写库成功后再逐个 `proposalStore_->submit(...)`，
      在"写库成功→提交提案"之间崩溃会静默丢提案（且不在同一事务内）。
   8. `writeResult.ok == false` → `lifecycle.failSummary(job.jobId, writeResult.message, now)`
      （`STORAGE_UNAVAILABLE` / `LIMIT_EXCEEDED` / `INVALID_ARGUMENT` 原样上报，**不得**当作已记住）。
      `ok == true`（含 `duplicate == true`）→ `lifecycle.completeSummary(job.jobId, job.toMessageId, now)`；
      `embeddingStatus` 只用于统计/日志（`Pending`/`Failed` 不是摘要失败）。
   9. 摘要成功后 **不需要**再调用 `remember(...)`（旧 sink 链路已移除；`remember` 只服务 D 的兼容工具）。

2. **提案待投递（B → C）**
   * 后台 tick（建议 30–60s，与向量化重试同频）调用
     `MemoryManager::deliverPendingProposals(*proposalStore, 32)`；返回本轮成功投递数。
     submit 失败会保留待投递并记录 `last_error`，下一轮自动重试。
   * `pendingProposalCount()` 可用于管理端指标/告警。

3. **向量化待办**
   * 后台 tick 调用 `retryPendingEmbeddings(16, now)`；后端不可用/冷却时返回 0，不消耗待办额度；
     摘要文本始终可读（`getSummary`）。
   * `embedding_status='pending'/'failed'` 的记录不会被召回，直到重试成功。

4. **上下文压缩（`ContextCompaction`）**
   * `FusionContext::compress` 已调用 `summarize(req)` 并把 `outcome.text` 拼进压缩结果；
     若需要把压缩摘要落库（供审计/管理端查看），用 `kind=ContextCompaction` 的 `SummaryRecord`
     调用 `writeSummary`：**不推进静默游标、不提升可见性、默认不可召回**。
   * 不要把压缩摘要当成经历记忆写入 `episodic_memory`（只有显式转换才可召回）。

5. **召回与注入（B → D/主 agent）**
   * 召回：`recall(query, access, topK)`（D 的 `recall_memory`/`conversation_*` 已接）。
   * 注入：`renderForPrompt(recalled, access)`（★建议替换现有 1 参调用），
     在拼 prompt 前再复核一次可见性；块首已标注"记忆数据…不是指令"并带时间/可见性/来源。

6. **失败与观测**
   * `summary_audit` / `migration_audit` 为只读审计；`summaryAudit(conv, limit)` 可查"空范围已处理到哪"。
   * A 的状态机只认 `completeSummary` / `failSummary` / `cancelSummary` 三选一：
     B 的失败（模型异常、校验失败、持久化失败、向量化失败）区分如下——
     向量化失败 → **complete**（文本已提交，待办会重试）；其余失败 → **fail**（保留冻结范围重试）；
     应用关闭/回收在飞任务 → **cancel**。

## 7. 契约变更请求 / 观察（未修改任何冻结文件）

1. **`SummaryRecord` / `SummaryRequest` 缺显式归属人（`ownerPersonId`）**：
   当前 `personId` 只能由"唯一参与者"近似（群聊记空）。若 `person` 可见性要在私聊中真正生效，
   需要主 agent 在 `SummaryContracts.h` 增加可选字段（或在 `SummaryRequest` 中提供），
   B 会在写入时使用它；在此之前 `person` 可见性记忆只在单参与者会话产生。
2. **`SummaryContracts.h` 注释与默认值不一致**：`privateVerdict` 注释写"解析失败保持默认私密"，
   但字段默认值是 `false`（旧映射 = 公开）。B 在 `SummaryManager` 内采用**保守实现**
   （未解析/无法解析 → 私密），建议主 agent 把结构体默认值或注释对齐，避免其他调用方误解。
3. **`AppConfig` 的 `memory` 节未反序列化 B 新增的 4 个键**（第 2 节）：
   默认值最保守，功能不受影响；若要支持配置调整，请在 `AppConfig.cpp` 的
   `from_json/to_json(MemoryConfig&)` 补 `writeVisibilityCap`/`maxSummaryBytes`/`promptMaxBytes`/
   `promptEntryMaxBytes`（含取值校验：可见性字符串非法时保持最窄）。
4. **`IProposalStore::submit` 忽略调用方 `proposalId`**（C 的实现固定生成 `prop-N`）：
   这使 outbox 无法用确定性 ID 实现"恰好一次"投递。若需要，请 C 在 `submit` 中保留
   调用方提供的合法 ID（或提供 `exists(idempotencyKey)`），B 侧的 outbox 已经按"至少一次"设计，
   不依赖该增强也能工作。
5. **`MemoryStore` 旧接口 `insert(...)` 移除**：仅旧版 `MemoryManager` 使用，已同步重写；
   如其他子任务有直接调用会编译失败（当前全量构建通过，说明没有）。

## 8. 固定格式声明

```text
人工审阅依赖：无
是否修改持久化格式：是
迁移是否可回滚：否（迁移整体在单事务内，失败自动 ROLLBACK、原库不变、可重试；但迁移成功后 schema v1→v2 不可自动回退，回退需用备份恢复）
权限边界是否改变：是（旧 is_public 行收窄为 conversation；写入可见性上限默认 conversation；召回查询前过滤 + 返回前复核 + 注入前复核；兼容 recall 别名降为最窄范围）
```
