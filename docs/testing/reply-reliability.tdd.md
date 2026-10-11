# 回复、QQ 消息和通讯录修复验证

基线：MIO `dev`，提交 `abd6b02491a50f1995716ddccab3e2f409a54942`。

回归场景从用户报告与现有调用链中提取，未单独生成规划文档。

## RED / GREEN 证据

修复前，`tests/lifecycle_test.cpp` 的最初 7 个行为回归在原实现上编译运行后全部失败：冷读取工具配对、压缩工具配对、残缺历史补齐、全角冒号 UTF-8、未回答图片压缩、工具调用保留图片、中断工具批次。

修复后增加摘要故障、沉默/强制总结边界、通讯录、消息发送和集成测试。最终编译与测试命令：

```sh
cmake --build ../build-full --parallel 4
ctest --test-dir ../build-full --output-on-failure
```

验证环境：Windows、MinGW-w64 GCC 15.2.0、C++17、CMake 4.3.3、nlohmann/json 3.12.0、cpr 1.12.0、IXWebSocket 12.0.1、SQLite 3.49.1。

完整应用 `MIO` 编译成功；最终 CTest 输出：

```text
1/6 Test #1: lifecycle ... Passed
2/6 Test #2: contacts  ... Passed
3/6 Test #3: mentions  ... Passed
4/6 Test #4: delivery  ... Passed
5/6 Test #5: runtime   ... Passed
6/6 Test #6: transport ... Passed
100% tests passed, 0 tests failed out of 6
```

HTTP 测试访问它自己在 `127.0.0.1` 启动的服务，受限执行环境初次拒绝回环连接；放行本机回环后单项与完整套件均通过。测试不访问真实模型或 QQ。

## 行为保证

| 测试文件 | 验证行为 |
| --- | --- |
| `tests/lifecycle_test.cpp` | 工具历史配对/修复；中文摘要解析；图片保留；取消工具补结果；沉默和强制总结完成状态；摘要故障回退 |
| `tests/contacts_test.cpp` | 联系人工具注册、新增/更新、重名、姓名绑定 QQ、平台别名、昵称与账号碰撞、落盘重读 |
| `tests/mentions_test.cpp` | 数组/CQ 事件、实体转义、媒体与消息 ID、结构化 @/引用、@QQ 映射及伪提及过滤 |
| `tests/delivery_test.cpp` | 模型工具过滤保留元数据、发送失败回报、私聊目标、引用白名单、输入缓冲与档案元数据 |
| `tests/runtime_test.cpp` | 实际 Runtime 中断及失败后图片保留/重试、完成后清理、工具真实接线、分气泡、通讯录 Facts 刷新、@姓名提示 |
| `tests/transport_test.cpp` | 实际 HTTP 429 重试保持原始图片载荷，以及模型请求中的消息/引用 ID 标签 |

## 审查与限制

C++、通用代码与安全审查已完成。发现的提前清图、联系人裸指针读取、跨会话引用、昵称干扰账号、空总结落档和 Release 测试断言被关闭问题均已修正。最终 C++ 复核无新问题，`git diff --check` 通过。

未测量行覆盖率，未使用真实 NapCat/QQ 或模型供应商联调。临时多模态片段仍遵循现有不落盘规则，进程退出后无法从磁盘恢复 `ephemeral` 图片。
