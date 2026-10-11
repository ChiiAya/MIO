# QQ 引用、@ 与通讯录

模型使用 `send_message` 的结构化参数发送 @/引用，文本中的 CQ 码会按普通文字发送。

```json
{
  "text": "这条我看到了[SEP]补充一句",
  "mention_user_ids": ["123456"],
  "reply_to_message_id": "987654"
}
```

`mention_user_ids` 接受 QQ 号字符串，仅支持 QQ 群聊。`reply_to_message_id` 必须取自当前物理会话消息的 `[message_id:...]` 标签；引用 ID 也会在入站标签中保留为 `[reply_to_message_id:...]`。其他群/私聊的引用 ID，即使出现在融合上下文中，也不能用于当前会话。

`target` 留空时发送到当前会话；使用 `private:QQ号` 或 `group:群号` 指定其他目标。只有第一个气泡附带 @/引用。平台发送失败时工具返回 `error:`，部分气泡已发送的情况也会明确提示。

通讯录工具：

| 工具 | 参数与作用 |
| --- | --- |
| `list_contacts` | 可选 `query`：按称呼、QQ、内部 ID 查询；省略时列出联系人 |
| `upsert_contact` | `name` 必填；可选 `qq`、`notes`；其他平台使用成对的 `platform`/`user_id` |
| `set_nickname` | `current` 指定 QQ、内部 ID 或唯一称呼；`nickname` 为新称呼 |
| `set_notes` | `person` 指定联系人；`notes` 替换备注，空字符串可清除 |
| `get_known_person_qq` | 保留旧工具；精确查询支持 QQ/内部 ID/唯一称呼，重名时要求区分 |

模型可先录入只知道名字的朋友，再在明确获知 QQ 后补齐账号。不同账号的同名联系人保持独立；账号标识的匹配优先于昵称。图谱沿用 `relationships.json`，兼容旧裸 QQ ID 和 `qq`/`napcat`/`onebot` 平台标签。

联系人更新会刷新 Facts；每轮请求也会为被 @ 的 QQ 附加当前名称与内部 ID，避免依赖旧的系统提示词缓存。

图片只在本轮已成功回复或选择沉默后清理。回复被新消息打断、模型调用失败或平台发送失败时，尚未完成的图片保留在内存中供下一轮继续处理。临时图片仍遵循现有 `ephemeral` 持久化规则，进程退出后不会从磁盘恢复临时图片。

回归测试运行方法见 [tests/README.md](../tests/README.md)。
