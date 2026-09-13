# 子任务 C 交付报告：身份事实、认知与关系提案

> 实现约束来源：`docs/operations/memory-system-refactor.md`（MUST / MUST NOT / 占位符约定）。
> 冻结契约（`src/core/contracts/**`）**未做任何修改**，无需契约变更。
> 全部测试不读写真实 `data/`：持久化文件一律写到 `std::filesystem::temp_directory_path() / "mio_test_c_<pid>"`。

```text
人工审阅依赖：HUMAN_REVIEW_REQUIRED
是否修改持久化格式：是
迁移是否可回滚：是
权限边界是否改变：否
```

四行说明：

- **人工审阅依赖**：`approveByHumanPlaceholder` / `rejectByHumanPlaceholder` 两个桩一律返回
  `HUMAN_REVIEW_REQUIRED`，`pending` 提案保持 `HUMAN_REVIEW_PENDING`，审核人字段恒为空、`reviewedAt` 恒为 0；
  没有人工审阅服务就无法把提案变成事实（这是设计，不是缺陷）。
- **持久化格式**：`relationships.json` 新增 `schema/version` 与关系状态字段（首次迁移自动备份）；
  新增 `proposals.json` / `facts.json` / `cognitions.json` 三个文件（各自带 `schema/version`）。
- **迁移可回滚**：迁移前生成逐字节备份 `relationships.json.bak-<epoch>`（已用真实文件副本验证 `cmp` 一致）；
  迁移只新增字段、不删除旧字段；回滚 = 停进程 → 删除新文件 → 用 `.bak-*` 覆盖回原路径。
- **权限边界**：未放宽任何权限；只做收紧（歧义昵称不再绑定、模型来源不能写 `confirmed`、
  模型来源不能提升可见性、关系类别只能经受控入口变更）。

---

## 1. 文件清单与设计要点

### 1.1 新增

| 文件 | 作用 | 关键设计 |
| --- | --- | --- |
| `src/mind/proposals/JsonStore.h/.cpp` | JSON 落盘公共工具 | 可注入 `EpochClock`（epoch seconds）；**原子写**（同目录 tmp → `fsync` → `rename`）；`backupIfAbsent`（同秒幂等）；读失败与"文件不存在"分开返回，便于区分"首次运行"和"数据损坏" |
| `src/mind/proposals/ProposalStore.h/.cpp` | `IProposalStore` 实现 | 一律落 `pending`；服务端强制覆盖 `status/factStatus/cognitionStatus/review/approvedBy/createdAt`；伪造审核人 → `INVALID_ARGUMENT` 且不落盘；写盘失败整体回滚并返回 `STORAGE_UNAVAILABLE`（绝不谎报"已记住"）；审计记录（时间/动作/actor 空/理由/结果）随库持久化 |
| `src/mind/proposals/FactStore.h/.cpp` | `IFactStore` 实现 | `addConfirmedFact` 是唯一 confirmed 入口（不注册为工具）；同一 `(subject_id, predicate)` 新值自动形成新版本并用 `supersedes_fact_id` 链接，旧版本置 `superseded` + `valid_to`；`supersedeFact` / `rejectFact` 都只改状态不删除；`listConfirmed` 三重过滤（confirmed + `valid_to==0` + 可见 + 非 legacy） |
| `src/mind/proposals/CognitionStore.h/.cpp` | 偏好/印象三态存储 | `observe()`（模型来源必须带证据）/`infer()`（模型主路径）只能写 observed/inferred；`addConfirmedCognition` / `confirmCognition` 拒绝一切 `model*` 来源；`importLegacyField` 写 `legacy_unverified=true` 的 inferred（保留数据但不注入）；撤回只置 `valid_to` |
| `src/mind/graph/GraphRelationshipStore.h/.cpp` | `IRelationshipStore` 适配器 | `get()` → `RelationshipState`；`bumpIntimacy` 走 `mio↔person` 事件增量；`setRelationshipType` / `setBlocked` 转发受控入口（**不注册为模型工具**） |
| `tests/test_facts_proposals.cpp` | 子任务 C 用例（11 个） | 覆盖 T08 / T11 与文档列出的全部验收点 |
| `docs/operations/subtask-C-report.md` | 本报告 | — |

### 1.2 修改（均在独占范围内）

| 文件 | 变更要点 |
| --- | --- |
| `src/mind/graph/RelationshipGraph.h/.cpp` | 节点新增 `relationship_type / familiarity / blocked / legacy_unverified / relationship_updated_at / interaction_events`；边新增 `last_interaction_at / interaction_count / version / event_keys`；新增 `noteInteraction`、带事件来源的 `bumpIntimacy`、`setRelationshipType`、`setBlocked`、`relationshipState`、`findByNameAll/findByNameUnique`、`reload/storageDegraded/lastLoadError`；`save()` 改原子写；旧格式迁移（备份 + 写回 + 保留未知字段）；**删除**"每条消息 +0.1 亲密度"路径 |
| `src/mind/facts/Facts.h/.cpp` | 保留旧签名；新增 facts/cognitions 重载；记忆数据独立成块并逐条标来源；只注入 observed+confirmed；不渲染 pending 提案与 `HUMAN_REVIEW_*`；不渲染原始亲密度浮点数；排序确定、两次重建逐字一致；`maxPeopleTokens` 预算保留 |

### 1.3 关键规则（为什么这么做）

1. **提案不是事实**：`ProposalStore` 只写 `pending`，事实层（`FactStore`）与关系层（`RelationshipGraph`）
   完全不知道提案的存在——所以"模型请求批准"在结构上不可能改变任何状态。
2. **confirmed 只有一个入口**：`addConfirmedFact` / `addConfirmedCognition`，且都拒绝 `model*` 来源
   （认知层）。它们不会被注册成工具（见 §5）。
3. **删除 = 状态转移**：`superseded` / `rejected` + `valid_to`，历史版本留在文件里，可 `get()` / `listForSubject()` 追溯。
4. **互动只更新统计**：`noteInteraction` 按 `eventSource` 去重、单次增量 ≤ 0.1、`intimacy ∈ [0,1]`、
   `version` 自增；熟悉度由**去重事件数**推导（≥3 → acquaintance，≥12 → familiar），
   **永不产生 family/friend**，且长期未联系不降级。
5. **family/friend 只能显式授予**：`setRelationshipType` 拒绝 `model*` 来源；`family`/`partner`
   只接受 `system_init / user_confirmed / human_review / human_review_service / admin`。
6. **迁移失败不破坏原文件**：解析失败时保留原文件、以空图继续，并把图谱置为**降级态**
   （`save()` 直接拒绝覆盖，`reload()` 可在人工修复后重试）。
7. **向前兼容**：迁移写回时保留不认识的根/节点/边字段（含 `attrs` 里的非字符串值），已用测试断言。

---

## 2. 配置示例（如需接线）

子任务 C **不修改** `src/config/**`（主 agent 独占）。存储构造参数只接受文件路径 + 可注入时钟，
不硬编码路径。建议在 `config.json` 新增（示例，不含任何凭据）：

```json
{
  "data": {
    "relationshipsPath": "data/relationships.json",
    "proposalsPath": "data/proposals.json",
    "factsPath": "data/facts.json",
    "cognitionsPath": "data/cognitions.json"
  }
}
```

读取后可这样构造（路径必须来自配置；时钟默认 `systemEpochClock()`，测试可注入假时钟）：

```cpp
mio::RelationshipGraph graph(cfg.data.relationshipsPath);            // 旧签名不变
mio::GraphRelationshipStore relStore(graph);
mio::ProposalStore proposals(cfg.data.proposalsPath);
mio::FactStore     facts(cfg.data.factsPath);
mio::CognitionStore cognitions(cfg.data.cognitionsPath);
```

文件格式（自动创建，人工无需预置）：

```json
{ "schema": "mio.proposals", "version": 1, "nextSeq": 3, "proposals": [ ... ], "audit": [ ... ] }
{ "schema": "mio.facts", "version": 1, "nextSeq": 2, "facts": [ ... ], "audit": [ ... ] }
{ "schema": "mio.cognitions", "version": 1, "nextSeq": 2, "cognitions": [ ... ], "audit": [ ... ] }
{ "schema": "mio.relationship_graph", "version": 2, "nextId": 6, "mioId": "mio", "nodes": [ ... ], "edges": [ ... ] }
```

---

## 3. 最小可复现验证步骤（确切命令）

```bash
cd /home/chiiaya/projects/MIO

# 1) 构建（与其它子任务共用构建锁）
flock /tmp/mio_build.lock xmake build mio_tests

# 2) 子任务 C 用例（按过滤器逐个跑）
./build/linux/x86_64/release/mio_tests 提案
./build/linux/x86_64/release/mio_tests 事实
./build/linux/x86_64/release/mio_tests 认知
./build/linux/x86_64/release/mio_tests 存储
./build/linux/x86_64/release/mio_tests T08
./build/linux/x86_64/release/mio_tests T11
./build/linux/x86_64/release/mio_tests 可见性
./build/linux/x86_64/release/mio_tests Facts渲染
./build/linux/x86_64/release/mio_tests 关系图谱

# 3) 全量（含其它子任务的用例，确认没有互相破坏）
./build/linux/x86_64/release/mio_tests
```

本次执行结果（release 构建，子任务 C 的 11 个用例全绿）：

```text
提案       [ DONE ] 运行 3 个用例，检查 63 次，失败 0 次
事实       [ DONE ] 运行 1 个用例，检查 31 次，失败 0 次
认知       [ DONE ] 运行 1 个用例，检查 24 次，失败 0 次
存储       [ DONE ] 运行 1 个用例，检查 10 次，失败 0 次
T08        [ DONE ] 运行 1 个用例，检查 23 次，失败 0 次
T11        [ DONE ] 运行 1 个用例，检查 27 次，失败 0 次
Facts渲染  [ DONE ] 运行 1 个用例，检查 24 次，失败 0 次
关系图谱   [ DONE ] 运行 1 个用例，检查 28 次，失败 0 次
```

全量套件（含其它子任务正在进行的用例）在本次交付时点了 57 个用例；其中 2 个失败来自
**子任务 F 的进行中用例** `tests/test_archive_coldstart.cpp`
（`F_ContextBuilder冷启动预算与当前回合优先`、`F_压缩分块覆盖全部目标范围_T07`），
断言的是 ContextBuilder/FusionUnit 的冷启动去重与分块覆盖，不经过 C 的任何文件
（C 未修改 `context/**`，也未改变 `FusionUnit`/`SummaryManager` 的任何接口）；
C 的 11 个用例与其余 44 个用例均通过。子任务 C 的结论以过滤器结果为准，不声称"全部通过"。

子任务 C 的 11 个用例：

```text
提案_提交为pending且审阅占位为空
提案_批准拒绝桩不改变状态并保留审计
提案_伪造审核人被拒绝且不落盘
存储_损坏文件保留不覆盖且拒绝写入
事实_确认与替代保留历史版本
认知_三态与注入过滤
T08_同名不错绑_自称管理员不改变状态
T11_旧图谱迁移与损坏恢复
可见性_记录遵守AccessContext
Facts渲染_确定性且标注为数据
关系图谱_互动不晋级且标签受控
```

**额外做过一次真实形态数据验证（未触碰 `data/`）**：把 `data/relationships.json` 复制到 `/tmp`，
用一次性驱动（`src/mind/graph/RelationshipGraph.cpp` + `src/mind/proposals/JsonStore.cpp` + `src/log/Log.cpp`）
加载，结果：14 人全部保留、`legacy_unverified` 全部为真、`relationship_type` 全空、
`relationships.json.bak-<epoch>` 与原文件 `cmp` 逐字节一致、新格式写回后 `reload` 仍为 14 人。
该一次性驱动不进仓库；**可复现路径以 T11 用例为准**。

---

## 4. 对现有接口的兼容性说明

| 接口 | 状态 | 说明 |
| --- | --- | --- |
| `buildSystemPrompt(persona, graph, cognition, maxPeopleTokens=1200)` | **旧签名保留** | `src/runtime/Runtime.cpp:552` 的现有调用无需修改即可编译（`xmake build mio_tests` / `xmake build MIO` 均已验证）。新增两个重载：`(..., int maxPeopleTokens, const IFactStore*, const CognitionStore*)` 与 `(..., const IFactStore*, const CognitionStore*, int maxPeopleTokens=1200)`，两者都不与旧签名产生二义 |
| `RelationshipGraph::bumpIntimacy(a, b, delta, now)` | **旧签名保留** | 新增 5 参重载（尾部 `eventSource`）。语义变化见下一条 |
| 亲密度增长语义 | **行为变更（有意）** | 删除 `onSeen` 里"每条消息 +0.1、上限 1.0"的晋级路径：现在每次**去重事件**最多 +0.1（默认 0.05），同秒同对人幂等，`[0,1]` 截断，`version` 自增。旧文件里的既有分数**原样保留**（不静默重置） |
| `relationships.json` 旧文件 | **仍可读** | 无 `schema` 即视为旧格式：备份 → 迁移 → 原子写回新格式；未知字段（根/节点/边/非字符串 attrs）原样保留；解析失败保留原文件并进入降级态，`reload()` 可重试 |
| `data/people.json` 旧迁移 | **仍可用** | 首次读取 `relationships.json` 不存在时继续从 `people.json` 迁移；迁移节点现在标记 `legacy_unverified=true` |
| `findByName(name)` | **行为变更（收紧）** | 重名（歧义）时返回 `nullptr`，绝不随便绑定其中一个；需要区分"不存在/歧义"用 `findByNameAll` / `findByNameUnique`。**注意**：`Runtime.cpp:442` 的 `get_known_person_qq` 有"子串匹配取第一个"的回退分支，那仍是一条可能绑错人的路径（属于主 agent 文件，见 §5） |
| `setNickname/setNotes/setPersonal/setLoves` | 兼容（更严格） | 名称歧义时返回 `false`；`setNickname` 还拒绝把目标改成与他人重名 |
| `topK(k)` | 兼容（更确定） | 权重降序，同权重按 `internalId` 升序 → prompt 前缀跨重启稳定 |
| **给 F 的接口确认** | — | `void RelationshipGraph::noteInteraction(const std::string& a, const std::string& b, const std::string& eventSource, std::int64_t now);` 签名与要求逐字一致；`a`/`b` 必须是已存在的内部 ID（未知 ID 不发明身份，记日志并跳过）；`eventSource` 建议 `convKey#messageId`；同一 `eventSource` 只计一次（跨重启也去重，保留最近 64 个事件键） |

### 测试中的模拟批准不是生产入口

用例只调用返回 `HUMAN_REVIEW_REQUIRED` 的桩来验证"状态不变"，没有为测试实现对生产可见的批准旁路
（生产代码里不存在任何把 `pending` 改成 `accepted` 的路径，只有 `markSuperseded` 这一个终态转移）。

---

## 5. 给主 agent 的 Runtime 接线清单

### 5.1 构造与生命周期（主 agent 负责）

```cpp
// AppConfig 增加路径字段（C 不改 config/**）
RelationshipGraph graph(cfg.data.relationshipsPath);          // 旧调用不变（可选第二参数：测试时钟）
GraphRelationshipStore relStore(graph);                        // 非拥有引用，graph 生命周期更长
ProposalStore   proposals(cfg.data.proposalsPath);
FactStore       facts(cfg.data.factsPath);
CognitionStore  cognitions(cfg.data.cognitionsPath);
```

三个 store 都为线程安全（内部互斥）、构造即加载、写盘原子；构造一次、全局共享，
**不要**每请求构造（每次构造都会重读文件）。

### 5.2 可以暴露给模型的接口（工具 handler 由 D 实现）

| 逻辑工具 | 可用接口 | 归属约束 |
| --- | --- | --- |
| `memory_propose_fact` / `memory_propose_preference` / `memory_propose_relationship` | `ProposalStore::submit(Proposal)` | `subjectId`/`conversationKey`/`source` 由**服务端上下文**填充（`AccessContext`），不接受模型冒填；返回值必须回传 `proposal_id`+`status`+`review_status` 给模型 |
| `memory_observe`（可选，低风险） | `CognitionStore::observe(rec, &id, &err)` | 模型来源必须带 `evidenceRefs`（`convKey#messageId`），否则 `INVALID_ARGUMENT` |
| 偏好/印象推断写入 | `CognitionStore::infer(rec, &id, &err)` | 落 `inferred`，不会进 prompt |
| `memory_list_pending`（管理员） | `ProposalStore::listPending(access, limit)` | 只返回当前会话可见 + 可见性达标的 pending；`limit==0` 返回空（防无界读取）；**绝不**放进默认 prompt |
| 事实/认知只读投影 | `FactStore::listConfirmed(access, limit)`、`CognitionStore::listInjectable(access, limit)` | 必须传真实 `AccessContext`；不要用 `listForSubject`（无权限过滤，只给审计/追溯） |

**绝不要注册成工具**（会破坏子任务 C 的硬约束）：
`ProposalStore::approveByHumanPlaceholder`、`rejectByHumanPlaceholder`、`markSuperseded`、
`FactStore::addConfirmedFact`、`addProposedFact`、`supersedeFact`、`rejectFact`、
`CognitionStore::addConfirmedCognition`、`confirmCognition`、`retract`、`importLegacyField`、
`RelationshipGraph::setRelationshipType`、`GraphRelationshipStore::setRelationshipType`、`setBlocked`。

### 5.3 Facts 重载如何被调用

```cpp
// Runtime::rebuildSystemPrompt（主 agent 修改）
systemPrompt_ = buildSystemPrompt(persona_, graph_, cognition, kMaxPeopleTokens,
                                  &facts_, &cognitions_);   // 指针顺序任选，两种重载都支持
```

注入规则（Facts 内部固定，**接线前务必确认**）：

1. 事实：`status==confirmed && valid_to==0 && !legacy_unverified`，且 `conversation_key` 为空、
   `visibility != conversation`；
2. 认知：`observed` 或 `confirmed`（`injectableIntoPrompt`），同样要求不绑定会话且非会话私有；
3. 人名/关系标签/熟悉度来自图谱 top-K（≤20，token 预算内），**不渲染亲密度/trust 浮点数**；
4. pending 提案在结构上不可能出现（渲染器拿不到提案）；
5. 排序全部确定（事实按 subject→predicate→createdAt→fact_id，认知按 subject→kind→id），两次重建逐字一致。

> ⚠️ 因为规则 1/2 排除了 `visibility == conversation` 与绑定会话的记录，
> **要让一条事实进入稳定 system 提示，写入时必须显式给 `visibility = person` 或 `public` 且不绑定会话**；
> 会话级事实应走"每会话召回注入"（当前阶段尚未实现，属 B/E 范围）。
> 若产品要求把会话级数据也放进 prompt，需要改成按会话重建 prompt（那时 `buildSystemPrompt`
> 需要新增 `AccessContext` 参数——属于未来契约变更请求，本次未做）。

### 5.4 迁移失败如何暴露

```cpp
if (graph.storageDegraded()) {
    log::error("Runtime", "关系图谱降级: " + graph.lastLoadError());
    // 建议：admin/health 上报 + 只读模式；不要自动改写原文件
}
// 人工修复文件后：
graph.reload();          // 成功返回 true 并清除降级态
```

三个 store 也有同样的 `degraded()` / `lastError()`；降级时写入返回 `STORAGE_UNAVAILABLE`（工具层应照此回传，
不得返回"已记住"）。迁移失败时磁盘上不会产生 `.tmp-*` 残留，也不会产生 `.bak-*`（备份只在解析成功后、
写回前发生）。

### 5.5 需要主 agent/F 注意的跨任务影响

1. **FusionRouter 仍在使用原始亲密度**：`src/context/conversationFusion/FusionRouter.cpp:240`
   `if (intimacy < cfg_.intimacyThreshold) return false;`。旧文件里 `intimacy` 普遍是 `1.0`
   （历史"每条消息 +0.1"刷满的结果），迁移**原样保留**；而新事件增长明显更慢（去重 + 0.05/次），
   因此"老对话仍能融合、新对话更难融合"。这是有意收紧（文档要求删除晋级路径），
   但需要主 agent/F 决定是否重置旧分数或改融合依据（C 不擅自删数据）。
2. `GraphRelationshipStore::get()` 的 `intimacyScore/trustScore` 取 `mio↔person` 边；
   `person↔person` 的边仍由图谱事件维护，不对外当关系类别。
3. `relationships.json.bak-<epoch>` 会落在数据目录里，admin 的文件列表/清理脚本需要容忍它。

---

## 6. 未完成项与数据风险

1. **旧分数不重置（数据风险）**：见 §5.5.1。旧 `intimacy=1.0` 不会被 C 静默清零；它不再渲染进 prompt、
   也不再能推出任何关系类别，但仍被 Fusion 阈值消费。
2. **旧图谱模糊迁移**：旧 `personal/loves/attrs` 只标记 `legacy_unverified`（保留原文、不注入 prompt、
   不成为 confirmed 事实）；`CognitionStore::importLegacyField` 提供了迁移入口，但**没有任何自动调用**
   （迁移不会凭空产生认知记录）。接线方若需要把旧印象转成认知，必须显式调用并逐条确认。
3. **`legacy_unverified` 没有自动解除路径**：只能通过 `confirmCognition`（用户明确确认）
   或重新 `addConfirmedFact`（不带 legacy 标记）解除；这是有意的（未验证字段不能自证）。
4. **`listForSubject` 无权限过滤**：它按设计返回全部版本（含 superseded/rejected/inferred/撤回），
   仅供审计/追溯；任何模型可读路径都必须改用 `listConfirmed` / `listInjectable`。
5. **`DiaryEntry` 旧路径没有三态**：`buildSystemPrompt` 仍渲染 `std::vector<DiaryEntry>`，
   标注 `来源：diary`，但 `DiaryEntry` 没有 observed/inferred/confirmed 概念。建议把日记写入迁移到
   `CognitionStore`（否则"只注入 observed+confirmed"这条保证对日记项不适用）。
6. **审计与事件键会缓慢增长**：`ProposalStore` 审计长期累积（当前不做归档/压缩）；
   每条边最多保留 64 个去重键，超出按 FIFO 淘汰（极端情况下很老的重复事件会再计一次）。
7. **`setRelationshipType` 的可信来源是硬编码白名单**
   （`system_init/user_confirmed/human_review/human_review_service/admin`）；未来若引入"自动规则可设 family"，
   需要显式改这一处并同步文档。
8. **未实现**：管理员 HTTP 端点（`src/admin/**` 属主 agent）、跨会话事实注入、事实向量化与召回、
   人工审阅服务本体——均不在 C 的范围。
9. **并发**：图谱返回的 `const PersonNode*` 仍是"下次写操作前有效"的借用指针（既有约定，未改变）。
10. **契约变更请求：无**。若未来需要按会话注入 prompt 事实，则需要给 `buildSystemPrompt` 增加
    `AccessContext`（由主 agent 决定，本次未改任何契约头）。

---

## 7. 覆盖到的验收项对照

| 验收编号 | 用例 | 结论 |
| --- | --- | --- |
| T08 | `T08_同名不错绑_自称管理员不改变状态` | 同名人各自独立内部 ID；`findByName` 歧义不绑定；平台 ID 精确定位；模型来源改关系类别被拒；提案批准桩不生效；confirmed 事实不变 |
| T11 | `T11_旧图谱迁移与损坏恢复` | 旧格式加载 → `legacy_unverified` 真、`relationship_type` 空、不产生 confirmed 事实；备份逐字节可回滚；未知字段保留；损坏 JSON 原文件不变、可 `reload` 重试、不崩溃 |
| 子任务 C「验收」 | `提案_*` / `事实_*` / `认知_*` / `可见性_*` / `Facts渲染_*` / `关系图谱_*` | 全部通过（C 自身 11 个用例，未受其它子任务进行中用例影响） |
