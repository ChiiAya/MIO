# 回归测试

测试不需要真实 QQ 账号或模型密钥。`lifecycle`、`contacts`、`mentions`、`delivery` 使用模拟模型/事件；完整构建还提供 `runtime` 集成测试，以及仅在本机回环地址启动模拟 HTTP 服务的 `transport` 测试。

基础测试只需要 C++17、CMake 3.20+、nlohmann/json 头文件：

```sh
cmake -S tests -B build/tests -DNLOHMANN_INCLUDE_DIR=/path/to/include
cmake --build build/tests --parallel
ctest --test-dir build/tests --output-on-failure
```

`NLOHMANN_INCLUDE_DIR` 应指向包含 `nlohmann/json.hpp` 的目录。Windows MinGW 可添加 `-G "MinGW Makefiles"`。Release 构建仍启用测试断言。

完整应用和集成测试使用项目现有的 cpr、IXWebSocket、SQLite3 依赖：

```sh
cmake -S tests -B build/tests-full -DMIO_BUILD_APP=ON -DNLOHMANN_INCLUDE_DIR=/path/to/include
cmake --build build/tests-full --parallel
ctest --test-dir build/tests-full --output-on-failure
```

cpr 和 IXWebSocket 的测试版本固定在 CMakeLists.txt。已有依赖源码可通过 `FETCHCONTENT_SOURCE_DIR_CPR`、`FETCHCONTENT_SOURCE_DIR_IXWEBSOCKET` 指定，避免重新下载。SQLite 可通过 CMake 的 `SQLite3_INCLUDE_DIR`、`SQLite3_LIBRARY` 指定。若使用自带 curl，可根据环境关闭可选依赖，例如 `-DCPR_USE_SYSTEM_LIB_PSL=ON -DCURL_USE_LIBPSL=OFF -DCURL_ZLIB=OFF`。

覆盖的行为：

- 冷读取与上下文压缩保留完整工具调用/结果，修复旧档案残缺工具批次。
- 中文/英文冒号摘要标记、摘要故障回退、未完成图片在压缩/工具调用后保留。
- 中断工具批次、保持沉默、强制总结的完成状态及空输出错误。
- 联系人录入、名称/备注更新、重名、账号与昵称碰撞、QQ 平台别名和重启持久化。
- OneBot 数组/CQ 字符串、实体转义、消息 ID、引用 ID、媒体和 @ 边界解析。
- 模型工具参数透传、发送失败回报、正确私聊目标、引用 ID 白名单。
- Runtime 中断重启思考、模型/平台发送失败后图片重试、成功后的图片清理。
- 分气泡时只在第一个气泡附 @/引用、新联系人刷新 Facts、@QQ 解析到当前名称。
- HTTP 429 重试保持图片及消息 ID/引用标签。

这些测试验证程序行为和协议载荷；真实 NapCat/QQ 与实际模型供应商仍需部署环境联调。
