# MIO 记忆系统改造 —— 交付说明与验收矩阵

> 依据：`docs/operations/memory-system-refactor.md`（实现约束文档；二者冲突时以该文档「补充执行契约」节为准）。
> 本文件记录「做了什么、怎么验证、什么没做、有什么风险」，是交付物的一部分。

## 1. 交付物清单

| 类别 | 位置 | 负责人 |
| --- | --- | --- |
| 共享契约（冻结） | `src/core/contracts/**`、`src/providers/memory/MemoryProvider.h` | 主 agent |
| A：配置与会话生命周期 | `src/context/lifecycle/**`、`src/config/AppConfig.*`、`src/config/ConfigManager.*` | A + 主 agent |
| B：摘要与经历记忆 | `src/context/summarizor/**`、`src/memory/**` | B |
| C：身份事实/认知/关系提案 | `src/mind/**` | C |
| D：模型记忆与会话查询工具 | `src/providers/llm/tool/handlers/**` | D |
| E：MemoryProvider 插件边界 | `src/providers/memory/**` | E |
| F：档案 ID / 冷启动 / 压缩 | `src/core/message/**`、`src/context/achieve/**`、`src/context/contextBuilder/**`、`src/context/conversationFusion/**` | F |
| 集成 | `src/runtime/**`、`src/main.cpp`、`src/admin/**`、`xmake.lua` | 主 agent |
| 配置示例（无凭据） | `config.example.json` | 主 agent |
| 测试 | `tests/**` | 全员 |
| 子任务报告 | `docs/operations/subtask-{A,B,C,D,E,F}-report.md` | 各子任务 |
| 改造前基线快照 | `docs/operations/baseline-state.md`、`baseline-before-refactor.patch` | 主 agent |

## 2. 构建与测试

```bash
cd /home/chiiaya/projects/MIO
xmake                       # 主程序 MIO
xmake build mio_tests       # 验收测试
./build/linux/x86_64/release/mio_tests          # 全部用例
./build/linux/x86_64/release/mio_tests <过滤词>  # 单个/部分用例
scripts/dev/verify.sh [过滤词]                   # 一键构建 + 跑用例
```

测试约束（文档要求）：不联网、不使用真实用户记录或付费 API；数据库使用临时目录；摘要/插件/LLM 使用 fake；调度使用可注入时钟。

**未运行真实应用进程**：`main.cpp` 目前把 `data/` 写死传给 `Runtime`，直接启动会触发真实 `data/memory.db` 的 schema v1→v2 迁移（按文档强制把旧 `is_public=1` 收窄为 `conversation`，且成功后不可自动回退）。这是用户数据的不可逆变更，交由使用者决定时机执行。集成验证因此完全由 `mio_tests`（含端到端用例）承担 —— 端到端用例会构造真实 `Runtime`、真实 SQLite 库与真实调度线程，只是数据目录在临时目录。

**升级前建议**：`cp -a data/memory.db* data/history data/relationships.json <备份目录>`。

## 3. 验收用例矩阵（文档「必须覆盖的验收用例」）

| 编号 | 场景 | 覆盖用例（`mio_tests` 过滤词） |
| --- | --- | --- |
| T01 | A 会话到期、B 会话仍活跃 | `T01_会话独立计时_只总结到期会话`、`T01_T03_端到端_只总结到期会话_新消息仍待总结` |
| T02 | follower 到达、生成中、旧定时器晚到 | `T02_follower与生成中不提前总结_旧任务晚到无效`、`A_待处理与生成上报不延长静默时间` |
| T03 | 总结中收到新消息 | `T03_总结中收到新消息_冻结范围提交_新消息仍待总结`、`T03_完成回报不得越过冻结终点`、`T01_T03_端到端_…` |
| T04 | 非法热更新、缩短/延长/关闭阈值 | `T04_非法热更新完整保留旧配置与调度行为`、`T04_缩短延长阈值与静默开关`、`T04_消息阈值边界`、`非法热更新_完整保留旧配置`、`会话配置_*`、`冷启动预算_*` |
| T05 | 写入后崩溃、模型超时、向量服务失败 | `T05_端到端_重启后游标正确且不重复摘要`、`B_T05_cursor_survives_restart_and_range_is_idempotent`、`B_T05_embedding_failure_keeps_text_and_retries`、`B_T05_model_timeout_does_not_advance_cursor`、`B_T05_missing_backend_keeps_pending_todo`、`A_持久化_*` |
| T06 | 冷启动、重新冷启动、历史检索 | `F_冷启动投影剔除reasoning与工具回合_T06`、`F_路由冷启动不调摘要LLM_T06`、`D_不返回reasoning与工具参数_T06` |
| T07 | 超长消息和超过前 20 条的历史 | `F_冷启动双重预算与UTF8截断_T07`、`F_压缩分块覆盖全部目标范围_T07`、`F_pendingRange只统计用户与可见助手` |
| T08 | 同名人、模型自称管理员、请求批准 | `T08_同名不错绑_自称管理员不改变状态`、`D_提案不改变事实层_T08`、`D_管理员列表权限_T08`、`提案_伪造审核人被拒绝且不落盘`、`提案_批准拒绝桩不改变状态并保留审计` |
| T09 | 摘要说公开、旧工具扩大可见性、伪造游标 | `D_跨会话读取被拒绝_T09`、`D_可见性只可收窄`、`B_T09_summary_cannot_widen_visibility`、`可见性_记录遵守AccessContext`、`D_证据ID服务端校验` |
| T10 | 重复 manual 请求与后续静默总结 | `D_manual幂等与静默游标解耦_T10`、`B_range_idempotency_not_by_summary_text` |
| T11 | 旧图谱/档案迁移中断或损坏 | `T11_旧图谱迁移与损坏恢复`、`存储_损坏文件保留不覆盖且拒绝写入`、`F_旧档案迁移_索引可重建且ID稳定`、`F_损坏行报告位置且后续消息可读` |
| T12 | 插件不可用、应用关闭时任务运行 | `E_T12_unavailable_provider_never_claims_success`、`E_T12_factory_selects_backend_and_degrades_safely`、`E_T12_provider_destruction_has_no_threads_or_dangling_state`、`E_plugin_sources_touch_no_archive_or_graph_files`、`T12_端到端_插件不可用时对话继续且可干净回收`、`T12_端到端_Hindsight占位不阻塞且无网络依赖`、`A_取消任务不消耗重试额度且可再调度`、`A_持久化_崩溃恢复把在飞任务转为可重试` |

调度测试使用可注入时钟（`IClock`），摘要与插件使用 fake，数据库与档案使用临时目录，未使用真实用户记录或付费接口。

## 4. 关键设计决定（与文档约束的对应）

1. **归属显式化**：静默摘要在后台线程执行，`SummaryJob` / `AccessContext` 显式携带会话、参与者、可见性与消息范围；`thread_local` 上下文只服务 ingest 线程内的同步工具调用（`set_public`、`get_current_user_qq` 等）。
2. **双游标**：调度器游标（`conversation_lifecycle.json`）与记忆库游标（`summary_cursor` 表）在 `completeSummary` 时对齐；崩溃窗口内重复请求同一范围由记忆库的 `(conv_key, from, to, summary_kind)` 范围幂等兜底。
3. **事务边界**：摘要行 + 摘要游标 + 向量化待办 + 提案待投递在同一 SQLite 事务（`BEGIN IMMEDIATE`）；**档案 JSONL 不参与该事务**，它是"已确认落盘"的上游事实源，不存在"JSONL 与 SQLite 原子"的说法。
4. **可见性只可收窄**：写入上限 `memory.writeVisibilityCap` 默认为 `conversation` 且配置层禁止设为 `public`；`set_public` 工具的扩大可见性调用被拒；摘要产出物恒为 `conversation`。
5. **人工审阅**：当前只实现数据字段、pending 持久化、审计记录与返回稳定错误码的服务桩；`approveByHumanPlaceholder` / `rejectByHumanPlaceholder` 一律返回 `HUMAN_REVIEW_REQUIRED` 且不改变业务状态。
6. **reasoning 隔离**：冷启动投影、历史查询工具、摘要材料、插件输入四条路径都剔除 reasoning 与完整工具参数，且不提供 `include_reasoning` 开关。
7. **插件边界**：`MemoryProvider` 是唯一的经历记忆后端接口（未并列新增 `MemoryBackend`）；本地 `sqlite-local` 为默认后端，`unavailable` / `hindsight` 恒不可用。`available() == false` 时召回工具返回空结果集 + `degraded` 标记（降级为无长期召回），对话主链路继续。工厂永不返回空指针、永不抛异常。
8. **配置失败即整体失败**：新字段（`conversation.*`、`contextBuilder.coldStart*`、`memory.backend` / `writeVisibilityCap` / 预算）类型或范围错误时，`ConfigManager` 不发布新快照、`Runtime` 不切换服务；热更新以当前配置为基底，缺失字段保持原值。

## 4.1 后台任务与关闭顺序

- 静默调度是 **pull 模型**：单个维护线程每 1s 枚举已有档案会话 → `shouldSummarize` → `tryBeginSummary`（原子冻结范围）→ 同步执行摘要 → `completeSummary` / `failSummary`。不为每条消息建线程。
- 每 60 tick 做一次低频维护：向量化待办重试、提案 outbox 投递、调度状态落盘。
- 关闭顺序：`stopMaintenance()`（置停止位 → join → 排空提案 → `persist()`）→ 停管理服务 → 成员按声明逆序析构，保证在飞任务不会访问已释放的 store/LLM。

## 5. 人工审阅占位现状

- 已实现：`Proposal.status`（pending/accepted/rejected/superseded）、`Fact.status`（proposed/confirmed/rejected/superseded）、独立 `reviewStatus`（`HUMAN_REVIEW_PENDING`）、审计记录、稳定错误码。
- 未实现（占位）：人工审核产品、审批入口、通知、跨平台合并审阅、认知确认策略、关系变更人工审阅。
- `HUMAN_REVIEW_ACTOR` / `HUMAN_REVIEW_REASON` / `HUMAN_REVIEW_TIMESTAMP` 仅为待接入字段：当前 `actor` 恒为空、`reviewedAt` 恒为 0，子模块不得伪造。
- `HUMAN_REVIEW_REQUIRED` 是**错误码**，`HUMAN_REVIEW_PENDING` 是**审阅状态**，二者不混用。

## 6. 未完成项

| 项 | 状态 | 说明 |
| --- | --- | --- |
| Hindsight 真实接入 | `HINDSIGHT_ADAPTER_TODO` | 配置、部署、联网传输、真实 SDK 均未实现；仅占位实现 |
| 人工审核产品 | 占位 | 只实现字段、pending 持久化与返回 `HUMAN_REVIEW_REQUIRED` 的服务桩 |
| 跨会话历史查询 | 未开放 | 当前默认仅限当前物理会话；`allowCrossConversation` 恒为 false |
| 插件进程隔离 | 未实现 | C++ 接口约束不构成沙箱；防不可信插件需另行进程隔离 |
| 关系类别自动升级 | 未实现（有意） | 只提供受控入口；模型只能提交 pending 提案 |

详见各子任务报告的「未完成项与数据风险」小节。

## 7. 主要数据风险（汇总）

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| 旧 `memory.db` 自动迁移 | 旧 `is_public=1` 行按文档强制收窄为 `conversation`（跨会话召回能力永久消失）；schema v1→v2 成功后不可自动回退 | 迁移在单事务内、失败自动 ROLLBACK 原库不变；升级前建议 `cp -a data/memory.db*` 备份 |
| 旧 `relationships.json` 迁移 | 首次加载会备份为 `relationships.json.bak-<epoch>` 后原子写回新格式；旧 `personal/loves/attrs` 标记 `legacy_unverified`，旧亲密度分数原样保留 | 解析失败保留原文件、空图继续、可 `reload()` 重试；未知字段全部保留 |
| 旧档案 ID 旁路索引 | `<file>.idx.json` 与 `_conversations.json` 是派生文件 | 删除后可重建（ID 由文件内容确定性推导）；旧命名文件需该会话来一条消息重新登记才能枚举 |
| 双游标崩溃窗口 | 调度器游标与记忆库游标在 `completeSummary` 时对齐 | 记忆库 `(conv_key, from, to, summary_kind)` 范围幂等兜底，重复提交返回原记录 |
| 旧亲密度分数偏高 | 历史"每条消息 +0.1"刷到 1.0 的分数被保留，仍参与 `FusionRouter` 的融合阈值判断；新事件增长更慢 | 不静默删除历史数据；分数不再进 prompt、不再推出关系类别；如需重置换算规则请单独立项 |
| 提案 outbox 为"至少一次" | `ProposalStore::submit` 固定生成 `prop-N`，调用方 ID 被忽略 | 投递成功才标记 delivered，重复投递可能产生重复 pending 提案（需 C 支持调用方 ID 才能做到恰好一次） |
| 群聊记忆无单一归属人 | `SummaryRecord` 无显式 `ownerPersonId`，群聊 `personId` 记空 | 宁可不归属也不冒填身份；`Person` 可见性在群聊不生效 |

## 8. 权限边界变更声明（集成检查项）

本次改造**收紧**了权限边界，未放宽任何一项：

- 模型首次获得**仅限当前物理会话**的历史读取工具；跨会话一律 `NOT_FOUND_OR_FORBIDDEN` 且不泄漏存在性。
- 旧工具 `remember` / `recall_memory` / `set_nickname` / `set_notes` 全部改走同一校验层；`set_public` 只能收窄，扩大可见性的调用被拒。
- 写入可见性上限默认为 `conversation` 且配置层禁止设为 `public`；召回在查询前、返回前、注入前三道闸门复核。
- 昵称重名 fail-closed（不再"子串匹配取第一个"而绑错人）。
- 模型不能审批、不能写 `confirmed` 事实、不能直接改关系类别、不能扩大可见性。
