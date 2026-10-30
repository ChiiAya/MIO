#pragma once
#include <stddef.h>
#include <stdint.h>

#define MIO_PLUGIN_ABI_VERSION 1

#if defined(_WIN32)
#  define MIO_EXPORT __declspec(dllexport)
#else
#  define MIO_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* data;
    size_t size;
} mio_str; /* 借用，不转移所有权 */

typedef enum {
    MIO_LOG_DEBUG = 0,
    MIO_LOG_INFO  = 1,
    MIO_LOG_WARN  = 2,
    MIO_LOG_ERROR = 3
} mio_log_level;

/* 宿主把结果写回时用的回调（避免跨边界 malloc/free） */
typedef void (*mio_write_fn)(void* sink, mio_str bytes);

/* ---- 插件 → 宿主 能力表 ---------------------------------------------- */
typedef int (*mio_tool_fn)(void* user, mio_str args_json, void* sink, mio_write_fn write);

typedef struct mio_channel_vtbl {
    uint32_t struct_size; /* 前向兼容：新字段只追加 */
    /* outbound_json: {source, channelId, text, replyTo, mentions[], parts[]} */
    int  (*send)(void* user, mio_str outbound_json, void* sink, mio_write_fn write);
    /* 可选：能力声明 {"reply":true,"mention":true,"maxLen":4500,...} */
    void (*capabilities)(void* user, void* sink, mio_write_fn write);
} mio_channel_vtbl;

typedef struct mio_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void*    ctx; /* 每个插件一个，宿主据此识别 owner */

    void (*log)(void* ctx, mio_log_level lv, mio_str tag, mio_str msg);

    /* 入站：Event JSON（沿用 Event::toJson 的字段） → AttentionGate */
    int  (*post_event)(void* ctx, mio_str event_json);

    /* 工具：def_json = {name, description, parameters, terminal} */
    int  (*register_tool)(void* ctx, mio_str def_json, mio_tool_fn fn, void* user);
    int  (*unregister_tool)(void* ctx, mio_str name);

    /* 出站通道：一个 source 对应一个通道 */
    int  (*register_channel)(void* ctx, mio_str source,
                             const mio_channel_vtbl* vt, void* user);

    /* 调用其他工具（含 MCP）—— 例如插件想写长期记忆 */
    int  (*invoke_tool)(void* ctx, mio_str name, mio_str args_json,
                        void* sink, mio_write_fn write);

    /* 数据目录：<dataDir>/plugins/<id>/ */
    void (*data_dir)(void* ctx, void* sink, mio_write_fn write);

    /* 宿主控制辅助（供 console 等管理插件调用） */
    int  (*reload_config)(void* ctx, mio_str config_path);
    void (*state_json)(void* ctx, void* sink, mio_write_fn write);
    void (*request_shutdown)(void* ctx);
    void (*flush_mind_loop)(void* ctx, void* sink, mio_write_fn write);
} mio_host_api;

/* ---- 宿主 → 插件 生命周期 ------------------------------------------- */
typedef struct mio_plugin_desc {
    uint32_t    struct_size;
    uint32_t    abi_version; /* 必须 == MIO_PLUGIN_ABI_VERSION */
    const char* id;          /* "napcat" */
    const char* version;     /* "0.1.0" */

    void* (*create)(const mio_host_api* host, mio_str config_json);
    int   (*start)(void* self);                /* 必须非阻塞：自己起线程 */
    void  (*stop)(void* self);                 /* 必须 join 完所有自有线程 */
    void  (*destroy)(void* self);
    /* 可选：配置热更新，返回非 0 表示需要重启插件 */
    int   (*reconfigure)(void* self, mio_str config_json);
} mio_plugin_desc;

/* 动态加载模式下的唯一导出符号 */
MIO_EXPORT const mio_plugin_desc* mio_plugin_entry(void);

#ifdef __cplusplus
}
#endif
