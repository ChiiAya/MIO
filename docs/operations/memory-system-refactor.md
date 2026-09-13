# MIO 记忆系统改造：子任务分发与强约束操作规范

> 本文是实现约束，不是开放式设计建议。子 agent 必须遵守其中的 MUST、MUST NOT 和占位符约定；若实现需要违反约束，先提交变更说明，不得自行扩大范围。

## 目标

将 MIO 的记忆系统拆成四个边界清晰的部分：

1. 身份事实：可追溯、默认不可直接覆盖；
2. 关系状态：亲密度、信任度和关系类别；
3. 经历记忆：对话沉默后的摘要和长期召回；
4. 会话档案：保存原始消息，并通过只读工具供模型查询。

Hindsight 将来只作为经历记忆后端插件接入，不直接接管身份事实、关系权限、审批或 prompt 预算。

## 总体不变量

- reasoning 永远不进入冷启动原始上下文、经历摘要输入或模型查询工具的默认返回值。
- 身份事实不原地覆盖。新值必须带来源，形成新版本或待审批提案。
- 模型写入先进入 proposal；只有经历摘要和低风险认知可以按策略自动接受。
- 关系类别是可变状态；身份事实是版本化事实。
- 所有记忆写入必须有会话范围、参与者、可见性和来源。
- 摘要必须按消息范围幂等，不能只按摘要文本去重。
- 工具查询遵守当前会话和参与者权限，默认不返回 reasoning、完整内部工具参数或其他会话私密内容。
- 任何模块 MUST NOT 通过 `const_cast`、绕过 store 或直接修改 JSON 文件来规避状态和权限检查。
- 所有时间均使用 epoch seconds；计时器内部可使用 monotonic clock，但落盘时间不得使用 monotonic 值。
- 所有新增持久化结构必须包含 schema/version 字段或可无损迁移的默认值。

## 强制状态与人工审阅占位符

人工审阅功能暂不实现，但接口和状态必须预留。以下占位符是协议的一部分：

```text
HUMAN_REVIEW_REQUIRED
HUMAN_REVIEW_PENDING
HUMAN_REVIEW_APPROVED
HUMAN_REVIEW_REJECTED
HUMAN_REVIEW_ACTOR         // 未来填入管理员/审核人 ID
HUMAN_REVIEW_REASON        // 未来填入审核理由
HUMAN_REVIEW_TIMESTAMP     // 未来填入审核时间
```

当前版本的默认行为：

- 需要人工审阅的提案 MUST 持久化为 `pending`，不得静默丢弃。
- `pending` 提案 MUST NOT 注入稳定 system prompt。
- `pending` 提案 MAY 在管理员专用列表中显示。
- 未实现审批入口时，任何“批准”调用 MUST 返回 `HUMAN_REVIEW_REQUIRED`，不得自动批准。
- 拒绝、批准和过期均必须保留审计记录；即使审阅模块尚未实现，也要保留字段。
- 子 agent 不得伪造 `HUMAN_REVIEW_ACTOR` 或将模型身份写入该字段。

建议预留接口：

```cpp
struct ReviewPlaceholder {
    std::string status;      // HUMAN_REVIEW_* 或空
    std::string actor;       // 当前版本必须为空
    std::string reason;      // 当前版本可为空
    std::int64_t reviewedAt = 0;
};

ReviewResult requestHumanReview(const ProposalId& id);
ReviewResult approveByHumanPlaceholder(const ProposalId& id);
ReviewResult rejectByHumanPlaceholder(const ProposalId& id);
```

`approveByHumanPlaceholder` 在人工审核服务接入前 MUST 返回错误，不得改变业务状态。

## 配置约定

在 `AppConfig` 中新增独立配置节，不把“对话结束”混入输入防抖配置：

```cpp
struct ConversationLifecycleConfig {
    int silenceTimeoutSeconds = 120;
    bool summarizeOnSilence = true;
    int minMessagesBeforeSummary = 2;
    int maxPendingSummaryRetries = 3;
};
```

配置文件节名为 `conversation`。`InputBufferConfig::debounceMs` 只负责合并短时连续消息；`conversation.silenceTimeoutSeconds` 负责判断一轮对话结束。

配置值必须校验：静默时间不得小于 1 秒；消息数和重试次数不得为负；非法热更新保持旧配置并记录日志。热更新只影响未触发的计时器；已开始的摘要任务继续运行。

配置约束：

- `silenceTimeoutSeconds` MUST 为整数，范围暂定 `1..86400`。
- `minMessagesBeforeSummary` MUST 为 `0..1000`。
- `maxPendingSummaryRetries` MUST 为 `0..10`。
- 缺少配置项使用默认值；类型错误或越界时整次配置重载失败，不能部分套用。
- 环境变量只覆盖对应字段，不得清空 JSON 中其他字段。
- 私聊和群聊暂时使用同一阈值；若要增加按会话类型覆盖，必须另行更新本文档和验收项。

## 子任务 A：配置与会话生命周期

### 范围

- 修改 `src/config/AppConfig.h/.cpp`，添加 `ConversationLifecycleConfig` 的 JSON 序列化、反序列化和环境变量支持。
- 新增按 `ConversationKey` 维护状态的 `ConversationLifecycle` 或 `SilenceScheduler`。
- 在 `Runtime` 中接入消息活动、静默计时和热更新。

### 会话状态

至少维护：

```text
lastActivityAt
lastSummarizedMessageId
summaryPending
activeGeneration
```

静默任务到期后必须重新检查：

```text
lastActivityAt + silenceTimeout <= now
AND pending messages > 0
AND activeGeneration == false
```

如果模型仍在生成，延迟摘要，不并发读取半成品对话。

### 验收

- 每个会话独立计时；新消息会取消或使旧任务失效。
- 热更新能改变后续静默判断，不重复处理已总结消息。
- `debounceMs` 和静默时间互不影响。
- 同一会话最多只能有一个有效静默任务；旧任务即使晚到也不得触发摘要。
- 摘要失败必须保留 `summaryPending` 和重试次数；不得把失败标记为已总结。
- 达到最大重试次数后必须进入可观察的失败状态，并保留 `HUMAN_REVIEW_REQUIRED` 或人工排障占位信息。

## 子任务 B：摘要与经历记忆

### 范围

- 将静默摘要与上下文压缩摘要区分为不同 `summary_kind`。
- 摘要输入只包含指定消息范围，过滤 reasoning。
- 摘要输出包含经历摘要、私密性、事实提案和关系变化提案。
- 为摘要增加 `fromMessageId/toMessageId`，实现消息范围幂等。

### 建议记录

```text
memory_id
conversation_id
participants
summary_kind
summary
visibility
created_at
event_time
from_message_id
to_message_id
source
```

强制字段：`from_message_id`、`to_message_id`、`summary_kind`、`source`、`visibility` 不得为空。`summary_kind` 只允许 `episodic_memory`、`context_compaction`、`manual` 三种值；未知值拒绝写入。

摘要写入顺序 MUST 为：读取范围 → 生成结果 → 校验结果 → 原子写入摘要和游标 → 再触发向量化。向量化失败不得回滚已保存的摘要，但必须标记 `embedding_status=pending/failed`，供后续重试。

现有 `MemoryManager` 的 `(convKey, summary)` 去重可以保留作兼容，但不能作为最终幂等依据。

### 验收

- 同一消息范围重复触发不会重复写入经历记忆。
- 上下文压缩不会自动变成长期经历记忆，除非明确标记。
- 私密摘要不会被公开会话召回。
- `context_compaction` 默认不得进入长期召回；只有显式转换为 `episodic_memory` 才可被召回。
- 摘要正文不得包含 reasoning、系统提示、API key、内部工具实现细节或人工审核占位字段。

## 子任务 C：身份事实、认知和关系提案

### 范围

新增事实/提案存储，或先在现有图谱旁增加兼容层。不要直接删除旧 `relationships.json`。

事实字段至少包括：

```text
fact_id, subject_id, predicate, object,
source, confidence, status,
created_at, valid_from, valid_to, approved_by
```

状态：`proposed / confirmed / rejected / superseded`。

认知（偏好、印象）必须区分 `observed / inferred / confirmed`。默认 prompt 只注入 `observed` 和 `confirmed`。

关系状态至少包括：

```text
relationship_type
intimacy_score
trust_score
confidence
source
updated_at
```

模型只能提交关系类别变化提案；亲密度可由事件增量和时间衰减更新，不能通过普通工具任意覆盖。

### 验收

- 旧图谱可读取，迁移失败不破坏原文件。
- 身份事实修改可追溯。
- 未确认推断不会伪装成稳定事实。
- 任何事实删除必须转为 `superseded` 或 `rejected`，不得物理删除唯一历史版本。
- `confirmed` 事实只能由系统初始化、用户明确确认或未来人工审核服务写入；模型普通工具不得写入。
- 关系提案不能直接改变 `relationship_type`；只有自动规则或未来人工审核服务可以提交状态变更。

## 子任务 D：模型记忆与会话查询工具

### 只读会话工具

建议工具：

```text
conversation.list_recent
conversation.search
conversation.get_messages
conversation.get_summary
```

默认参数：当前会话、有限条数、有限字符数、不包含 reasoning、不包含完整内部工具参数。返回结构化消息和分页游标。

### 写入工具

建议工具：

```text
memory.save_episode
memory.propose_fact
memory.propose_preference
memory.propose_relationship
memory.list_pending
```

普通模型上下文不开放任意 `approve` 工具；审批只能由管理员接口、人工确认流程或明确的系统策略触发。

### 验收

- 跨会话读取遵守参与者和可见性策略。
- 工具参数过大时拒绝或截断，不导致 prompt 无界增长。
- 模型可以主动保存经历，但不能直接覆盖确认事实。
- 所有写工具必须返回 proposal/memory ID 和状态，不得只返回自然语言“已记住”。
- 参数缺失、越权、超预算和不可见记录必须返回稳定错误码。

## 子任务 E：Hindsight 插件边界

将现有 `MemoryProvider` 扩展为经历记忆接口，例如：

```cpp
class MemoryBackend {
public:
    virtual ~MemoryBackend() = default;
    virtual void storeEpisode(const EpisodeMemory&) = 0;
    virtual std::vector<MemoryHit> recall(const RecallQuery&) = 0;
};
```

插件约束：

- 插件 MUST NOT 读取 MIO 原始档案目录、关系图谱文件或审批字段。
- 插件只接收已通过 MIO 权限过滤的 `EpisodeMemory` 和 `RecallQuery`。
- 插件返回的内容必须经过 MIO 的可见性过滤、长度限制和注入格式化。
- 插件不可用时，MIO 必须降级为无长期召回运行，不得阻塞主对话。
- 插件返回的事实或关系判断只能转成 proposal，不能直接写入确认层。

SQLite 实现作为默认后端；Hindsight 实现只负责经历存储、归纳和召回。身份事实、关系状态、权限、审批、原文档案和上下文预算仍由 MIO 控制。

## 推荐实施顺序

1. 先做配置和会话生命周期，不改变现有图谱格式。
2. 给摘要增加消息范围和 `summary_kind`。
3. 增加会话只读工具，并统一权限过滤。
4. 引入事实/关系提案模型。
5. 将 `RelationshipGraph` 的可变字段迁移到关系/认知层。
6. 扩展 `MemoryProvider`，最后接入 Hindsight。

## 暂不做

- 不在本阶段引入完整知识图谱数据库。
- 不允许模型直接审批自己的事实提案。
- 不把 reasoning 当作长期记忆材料。
- 不用单一亲密度浮点数替代关系类别、信任度和来源。
- 不在没有权限模型前开放全局历史搜索。

## 交付要求

每个子任务提交：

1. 代码和必要的数据迁移；
2. 配置示例；
3. 最小有意义的测试或可复现验证步骤；
4. 对现有接口的兼容性说明；
5. 未完成项和数据风险说明。

每个子 agent 还必须明确填写：

```text
人工审阅依赖：无 / HUMAN_REVIEW_REQUIRED
是否修改持久化格式：是 / 否
迁移是否可回滚：是 / 否
权限边界是否改变：是 / 否
```

若“权限边界是否改变”为“是”，由主 agent 在集成时检查本文约束；这不是要求用户另行批准开发工作的流程。

## 补充执行契约（与前文冲突时以本节为准）

### 已确认要求与实施默认值

用户已确认：关系图谱维护身份和社会关系、事实不可任意修改、沉默后总结、沉默时间由 AppConfig 调整、冷启动限量原文且剔除 reasoning、开放历史查询和自主记忆工具、未来插件化接入 Hindsight。

本文中的 120 秒、消息阈值、重试次数、工具预算和关系策略是可调整的实施默认值，不得描述为用户已逐项确认。当前交付是操作文档；人工审阅产品、真实 Hindsight 接入均作为后续占位，不在本阶段实现。

### 人工审阅与权限矩阵

业务提案状态统一为 `pending / accepted / rejected / superseded`；事实状态沿用 `proposed / confirmed / rejected / superseded`。两者不得混用。审阅字段使用独立 `reviewStatus`；`HUMAN_REVIEW_REQUIRED` 是错误码，`HUMAN_REVIEW_PENDING` 是审阅状态。`HUMAN_REVIEW_ACTOR/REASON/TIMESTAMP` 仅表示待接入字段，不得将这些字符串写成真实审核人或时间。

| 操作 | 当前版本行为 | 后续占位 |
| --- | --- | --- |
| 平台首次识别账号 | 系统建立内部 ID 和可信平台绑定 | 跨平台合并审阅 |
| 模型提出身份事实或修改 | 保存 pending，事实层不变 | HUMAN_REVIEW_PENDING |
| 对话中声称“已确认” | 保存声明和证据，不能授予审批权限 | 用户身份验证与确认入口 |
| 模型写偏好、印象 | 可存为 inferred，保留来源；不升为 confirmed | 认知确认策略 |
| 模型主动保存经历 | 校验后保存 manual 记忆，返回持久化 ID | 无 |
| 模型修改关系类别 | 仅保存 pending 提案 | 关系变更人工审阅 |
| 模型批准、拒绝或扩大可见性 | 拒绝，不改变状态 | 人工认证、审计接口 |

人工审阅当前只实现数据字段、持久化 pending 和返回稳定错误的服务桩。approve、reject 两个桩均返回 `HUMAN_REVIEW_REQUIRED`，不得创建管理网页、真实批准接口、自动批准超时或通知外部人员。审核人为空，审核时间为 0；不得把模型、普通聊天发送者或后台任务冒充审核人。测试中的模拟批准不得成为生产绕过入口。

摘要重试耗尽使用 `SUMMARY_RETRY_EXHAUSTED`，与人工事实审阅区分。不得因审阅产品尚未实现而停下其他已授权实现工作。

### 配置和调度的精确定义

- 初次启动按默认值 → 已提供的环境变量 → JSON 字段覆盖；局部热更新以当前配置为基底，缺失字段保持原值。环境变量重载仅覆盖实际存在的变量，不重置其他字段。
- 新字段类型或范围错误时，配置与相关服务均不发布新版本。先构造并校验候选配置和依赖，再切换运行状态。不得沿用捕获字段错误后静默成功的行为。
- 环境变量固定为 `MIO_SILENCE_TIMEOUT_SECONDS`、`MIO_SUMMARIZE_ON_SILENCE`、`MIO_MIN_MESSAGES_BEFORE_SUMMARY`、`MIO_MAX_PENDING_SUMMARY_RETRIES`。
- 会话按物理 ConversationKey 计时，不按共享 FusionUnit 计时。接收有效消息即更新活动版本，包括 InputBuffer follower；模型输出完成也更新活动时间。工具内部事件不反复延长静默时间。
- 等待条件必须同时包含：静默到期、存在未总结消息、满足消息阈值、无待处理输入、无生成请求、无该会话正在执行的摘要。消息阈值按用户及可见助手消息计数，不计 reasoning、工具和框架提醒；0 表示不额外限制，但空范围仍不能总结。
- 状态至少包括 `activityVersion`、待处理/生成数量、摘要任务 ID、已提交游标、重试次数。旧定时任务校验版本后退出；不为每条消息创建独立常驻线程。
- 摘要开始时冻结已落盘范围；之后的新消息继续正常处理并留待下次总结。旧摘要成功只能推进至冻结的结束 ID，不得清掉新消息的待处理标记。
- 后台工作必须显式携带会话、参与者、可见性和范围；禁止读取 `g_activeConv/g_activeMemoryPerson` 等 thread_local 作为归属。
- 缩短阈值后已到期会话安排一次检查；延长阈值则重算。关闭静默摘要停止新调度，已启动任务可完成；重新开启只处理未提交范围。
- 重试次数不含首次尝试；0 表示首次失败后不自动重试。持久化失败状态和冻结范围，重启不能无限刷新重试额度。析构先停止调度并回收任务，再释放 store/LLM。

### 档案、事务和失败恢复

- 档案需要稳定的会话内递增消息 ID；引用使用 `(conversationKey, messageId)`，不得用昵称或时间戳代替唯一身份。新字段兼容旧记录，不批量改写唯一原档案。
- 旧档案通过可重建的旁路索引分配稳定 ID；损坏行必须报告位置，不得把损坏后的消息静默当成不存在。路径编码必须防会话键碰撞，不能仅替换特殊字符。
- 只有确认落盘成功的消息可进入总结范围；先改缓存后忽略文件写入错误不得被视为成功。读取快照后释放锁，再调用模型或插件。
- 在本地同一事务中保存摘要、提案或提案待投递记录、摘要游标和向量化待办；禁止声称对 JSONL 和 SQLite 分别写入就是原子事务。
- 静默任务唯一键使用会话、起止 ID、summary_kind；提交时比较旧游标，防止重叠范围重复提交。相同范围重试返回原记录。
- manual 记忆有独立 idempotencyKey 和来源消息引用，不推进静默总结游标。模型只能提供允许读取的证据 ID；服务验证后赋予真实来源和可见性。
- 向量化失败保留文本及待办，后端返回 queued 必须有本地持久化依据；失败不得返回“已记住”。没有值得保留的内容也要有可审计的处理结果，不能产生无限重复任务。

### 冷启动、上下文压缩与 reasoning

- 新增子任务 F：修改 Achieve 冷读取、Fusion 冷启动/重新冷启动和 ContextBuilder。冷启动不得同步调用摘要模型或写入长期记忆。
- AppConfig.contextBuilder 增加 `coldStartRawTokens=2000` 和 `coldStartMaxMessages=20` 两个实施默认值，均允许 0 表示不恢复历史原文；校验非负且 token 预算不得超过总上下文预算。
- 选择最近可见消息，在双重预算内按时间正序恢复；保留来源、角色和时间，不重复加入当前批次。当前请求及输出预留预算优先；超长单条正文按 UTF-8 安全截断并标记，不能截断工具 JSON 后直接发送。
- reasoning 在冷启动、历史工具、摘要材料及插件输入中一律排除，不提供 include_reasoning 开关。处理消息副本，不破坏当前仍在进行的工具循环所需协议数据或既有档案。
- 冷启动默认恢复用户/助手可见正文；历史工具调用不作为未完成调用重放，避免留下孤立 tool 消息。有效工具证据需要时通过有界查询工具读取。
- 人设及可信固定配置可在 system；个人事实、印象和召回内容须明确标为带来源的数据，不能因位于 Facts 渲染器中而升级为行为指令。
- 上下文压缩可独立生成 context_compaction，但不写经历、不推进静默游标、不提升可见性。大范围分块必须覆盖全部目标消息，不得沿用“前 20 条加末尾 5 条”而无提示丢弃中间范围。

### 社会关系与可见性

- `family` 是亲属/角色关系，不是刷消息可升级的亲密度等级。拆为关系标签和熟悉程度：标签可含 family、friend；熟悉程度为 stranger/acquaintance/familiar。blocked 单独作为交互状态。
- 首次接触建立 stranger 状态；当前版本不实现自动升为朋友、亲人或自动提高信任。删除每条消息增加 0.1 就能接近满值的晋级路径；互动频率仅更新熟悉/活跃统计。
- 若实现连续亲密分数更新，规则必须有事件来源、去重、上下界和版本；不得自行把长期未联系解释为亲属/朋友关系失效。
- 身份写入以内部 ID 定位；昵称重名返回歧义，跨平台账号不因同名自动合并。旧 personal/loves/attrs 标记 legacy_unverified，不自动成为 confirmed；旧分数不能证明朋友或亲属关系。
- 当前阶段历史查询默认仅限当前物理会话。跨会话工具查询暂不开放；亲密度、trust 或摘要模型的“公开”判断都不能授权跨会话访问。
- 记忆默认 conversation 可见；摘要输出仅可建议收窄，不能自动扩大为 public。未知或解析失败保持最窄范围；旧 isPublic 记录在未确认来源前收窄并保留迁移记录。
- 召回在查询前过滤，并在返回和 prompt 注入前复核。不可见与不存在统一 `NOT_FOUND_OR_FORBIDDEN`，不得泄漏隐藏条数、标题或摘要。
- 现有 remember、recall_memory、set_nickname、set_notes、set_public 必须走同一校验层或安全兼容别名；不得保留可绕过新规则的旧入口。

### 工具和插件接口

- 前文点号工具名是逻辑名称；注册名统一为下划线形式，例如 conversation_get_messages、memory_propose_fact。所有者字段、当前会话、审核状态从服务端上下文取得，不接受模型冒填。
- 读取工具默认 20 条、最多 100 条、每次总计最多 12000 UTF-8 字节；写入正文最多 4000 字节，query 最多 1000 字节。超限写入返回 LIMIT_EXCEEDED；读取返回截断标记和游标。
- 游标须绑定会话和查询范围并在服务端校验。分页后继续执行权限检查。工具不接受任意文件路径、SQL 或脚本。
- 返回 JSON envelope：`{ok, data, error}`；错误码至少有 INVALID_ARGUMENT、LIMIT_EXCEEDED、NOT_FOUND_OR_FORBIDDEN、HUMAN_REVIEW_REQUIRED、STORAGE_UNAVAILABLE、CONFLICT。写入成功 data 含 ID 和状态；历史数据不作为可执行指令。
- 复用现有 MemoryProvider，不并列新增含义相同的 MemoryBackend。至少定义 storeEpisode、recall 和明确的成功/失败结果；输入输出携带本地 ID、来源 ID 和可见性元数据。
- E 本阶段仅交付接口、本地后端适配和 unavailable 测试桩。Hindsight 配置、部署、联网传输、真实 SDK 实现均为 `HINDSIGHT_ADAPTER_TODO`。
- 插件不得接收原档案目录能力或管理凭据。接口约束不等于进程隔离：若未来需要防不可信插件读取宿主文件，另行实现进程隔离，不得宣称当前 C++ 接口已提供沙箱。

### 分发、文件归属和依赖

| 负责人 | 独占修改范围 | 依赖与交付 |
| --- | --- | --- |
| 主 agent | Runtime、AppConfig/ConfigManager、main、admin、xmake.lua、共享契约头 | 先冻结契约；统一注册工具、接入生命周期和发布配置 |
| A | 新生命周期调度目录 | 提供配置结构和调度 API；向主 agent 提交 Runtime 接线清单 |
| B | summarizor、memory/store、memory/manager | 摘要事务、游标、向量化待办；依赖 F 的档案 ID 契约 |
| C | mind/graph、mind/facts、新提案及认知存储 | 图谱迁移、事实版本、审核桩和读取投影 |
| D | 新历史/记忆工具 handler 目录 | 依赖 B/C/F 接口；仅由主 agent 修改 Runtime 工具注册 |
| E | providers/memory 及本地适配目录 | B 完成后适配；不要与 B 同时编辑 store/manager |
| F | core/message、context/achieve、contextBuilder、conversationFusion | 档案 ID、投影、冷启动预算；共享配置变更由主 agent 合入 |

先由主 agent 确定 ArchiveRecord、SummaryJob、Proposal、AccessContext、MemoryProvider 契约；再并行分发 A/C/F，随后接入 B/D，最后 E 和主 agent 集成。agent 数不足时分波执行，不要求同时启动全部任务。

现有工作区有未提交修改。开始前记录状态，阅读归属文件的当前内容；不得 reset、覆盖其他任务改动或顺手重构。共享文件只能由指定负责人编辑，依赖变化以接口清单交接。示例配置另建无凭据样例，不复制真实 config.json。

### 必须覆盖的验收用例

| 编号 | 场景 | 必须结果 |
| --- | --- | --- |
| T01 | A 会话到期、B 会话仍活跃 | 只总结 A；后台结果归属 A |
| T02 | follower 到达、生成中、旧定时器晚到 | 不提前总结，不重复任务 |
| T03 | 总结中收到新消息 | 冻结范围成功提交，新消息仍待总结 |
| T04 | 非法热更新、缩短/延长/关闭阈值 | 非法更新完整保留旧状态；合法更新符合调度规则 |
| T05 | 写入后崩溃、模型超时、向量服务失败 | 重启游标正确；不重复摘要；文本可恢复且向量可重试 |
| T06 | 冷启动、重新冷启动、历史检索 | 不含 reasoning；冷启动不调摘要 LLM；不残留孤立工具消息 |
| T07 | 超长消息和超过前 20 条的历史 | 预算有效、截断可见；待总结范围不静默漏掉中间消息 |
| T08 | 同名人、模型自称管理员、请求批准 | 不错绑身份、不批准、不改变 confirmed 事实 |
| T09 | 摘要说公开、旧工具扩大可见性、伪造游标 | 保持原可见范围；无跨会话泄漏 |
| T10 | 重复 manual 请求与后续静默总结 | 主动记忆幂等；不抢占静默游标 |
| T11 | 旧图谱/档案迁移中断或损坏 | 原数据保留；迁移可重试；未验证字段不升级为事实 |
| T12 | 插件不可用、应用关闭时任务运行 | 对话可继续；任务回收后释放依赖，无悬空访问 |

调度测试使用可注入时钟，摘要和插件使用 fake，数据库使用临时目录。不得用真实用户记录或付费 API 验证。集成完成运行 xmake 构建及上述有意义测试；依赖不可用时报告具体未验证项，不得写“全部通过”。文档更新本身无需运行应用测试。
