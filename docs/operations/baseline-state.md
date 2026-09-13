# 改造前工作区基线状态（主 agent 记录）

记录时间：改造开始前。工作区存在未提交修改（用户既有改动），改造过程不得 reset/覆盖。

```
 M .gitignore
 M src/adapters/console/ConsoleAdapter.cpp
 M src/config/AppConfig.cpp
 M src/config/AppConfig.h
 M src/config/ConfigManager.cpp
 M src/context/contextBuilder/ContextBuilder.h
 M src/context/conversationFusion/FusionContext.cpp
 M src/context/conversationFusion/FusionContext.h
 M src/context/conversationFusion/FusionRouter.cpp
 M src/context/conversationFusion/FusionRouter.h
 M src/main.cpp
 M src/mind/facts/Facts.cpp
 M src/mind/facts/Facts.h
 M src/providers/llm/Llm.h
 M src/providers/llm/tool/ToolLoop.cpp
 M src/providers/memory/MemoryProvider.h
 M src/runtime/Runtime.cpp
 M src/runtime/Runtime.h
 M xmake.lua
?? docs/
?? src/admin/
?? src/core/contracts/
?? tests/
```

完整 diff 已另存为改造前快照（1037 行）。
HEAD：c16fafe75d904551626c5d462c24495b7316420f（分支 dev）
