#pragma once

// ============================================================================
// NapCat QQ 反向 WebSocket 适配器
//
// 连接模型（对应 NapCat onebot11 配置的 websocketClients）：
//   MIO 启动 WebSocket 服务端，NapCat 主动连入；同一条连接承载两个方向：
//     * 事件推送 NapCat → MIO —— OneBot 11 事件 JSON 帧
//     * API 调用 MIO → NapCat —— {"action","params","echo"} 帧（回复使用
//       官方统一 send_msg 接口，NapCat 以相同 echo 返回 status/retcode/data）
//
// 解析边界：
//   * 所有事件（message/notice/request/meta_event）都会先解析成
//     napcat::OneBotEvent，字段定义见 OneBotTypes.h；
//   * message 事件再转为 IncomingMessage 交给 Runtime；
//   * notice/request 当前暂不消费，但会 Info 记录完整关键字段，供后续扩展。
//
// 鉴权（OneBot 11）：NapCat 连入时握手 URL 带 ?access_token= 或
// Authorization: Bearer 头；MIO 配置了 token 而两者都不匹配 → 拒连（1008）。
//
// 环境变量（均有默认值）：
//   MIO_NAPCAT_HOST  反向 WS 监听地址（远程 NapCat 需 0.0.0.0）（默认 127.0.0.1）
//   MIO_NAPCAT_PORT  反向 WS 监听端口（默认 6199）
//   MIO_NAPCAT_TOKEN OneBot access token（空 = 不鉴权）
//
// 启用方式：MIO_PLATFORM=napcat（其余平台走控制台，见 main.cpp）。
// ============================================================================

#include "runtime/Runtime.h"

#include <string>

namespace mio {

struct NapCatConfig {
    std::string listenHost = "127.0.0.1";
    int listenPort = 6199;
    std::string token;  // OneBot 11 access token（空 = 不鉴权）

    // 从环境变量读取（不存在则用默认值）：
    //   MIO_NAPCAT_HOST / MIO_NAPCAT_PORT / MIO_NAPCAT_TOKEN
    static NapCatConfig fromEnvironment();
};

// 阻塞运行：启动反向 WebSocket 服务端 + 后台处理线程，直至进程退出。
// 事件按到达顺序串行 ingest（与档案追加顺序一致）；回复经同一连接
// 下发 action 帧，echo 匹配 NapCat 响应（超时/retcode 非 0 记为失败）。
void runNapCat(Runtime& runtime, const NapCatConfig& config);

} // namespace mio
