// momo_extension.h — Momo 原生扩展稳定 C ABI(v1)
// 设计说明: docs/07-extension-system.md。本头文件是扩展与宿主之间唯一契约:
//   * 纯 C、无异常、无 C++ 类型跨边界;
//   * 函数表与结构体只增不改不删, 所有结构体首字段为 struct_size 以便前后兼容;
//   * 线程约定标注在每个函数注释中, 违反即未定义行为;
//   * 回调线程规则: 宿主调用扩展的回调, 一律在扩展注册它的线程上执行, 或在注册时以 MomoTaskRunner
//     显式指定的线程上执行;宿主不会在自己的内部线程(raster、vsync、IO…)上调用扩展代码。
//     唯一的固定线程是 JS 相关回调(模块 setup、JS 函数), 它们属于 JS 运行时, 总在 ui 线程。
// 状态: v1 定稿(M5)。由 extensions/examples 下的 hello(函数/Promise/通道)与 texture(纹理注入/
// 帧回调)验证;hello 以纯 C 编译, 验证 ABI 不依赖 C++。
// v1 追加(M6): 平台视图工厂(platform_view_register_factory / platform_view_get), 由 webview 示例验证。
// 之后只能在 MomoHostApi / MomoCallApi 末尾追加字段(扩展用 struct_size 判断宿主是否提供)。

#ifndef MOMO_EXTENSION_H_
#define MOMO_EXTENSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define MOMO_EXT_EXPORT __declspec(dllexport)
#else
#define MOMO_EXT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MOMO_EXT_API_VERSION 1u

// ---------- 基本类型 ----------

typedef struct MomoEngine* MomoEngineRef;        // 引擎实例(不透明)
typedef struct MomoJsBuilder* MomoJsBuilderRef;  // 模块构建器(仅 setup 回调内有效)
typedef struct MomoCallInfo* MomoCallInfoRef;    // 一次 JS 调用(仅回调栈内有效)

typedef enum MomoStatus {
  kMomoOk = 0,
  kMomoErrInvalidArgs = 1,
  kMomoErrWrongThread = 2,
  kMomoErrNotFound = 3,
  kMomoErrVersionMismatch = 4,
  kMomoErrInternal = 100,
} MomoStatus;

typedef enum MomoTaskRunner {
  kMomoRunnerPlatform = 0,  // OS 主线程(平台 API 在此)
  kMomoRunnerUi = 1,        // JS 线程(禁止阻塞)
  kMomoRunnerIo = 2,
  kMomoRunnerWorkerPool = 3,
} MomoTaskRunner;

typedef enum MomoLogLevel { kMomoLogDebug, kMomoLogInfo, kMomoLogWarn, kMomoLogError } MomoLogLevel;

typedef void (*MomoWorkFn)(void* user_data);
typedef void (*MomoModuleSetupFn)(void* user_data, MomoJsBuilderRef builder);
// JS 原生函数回调: 在 ui 线程被调, 通过 call_info 读参数/写返回值/抛 JS 错误。禁止阻塞。
typedef void (*MomoNativeFn)(void* user_data, MomoCallInfoRef call_info);
// 消息通道 handler: bytes 仅在回调期间有效, 需要保留必须拷贝。reply_token 用于应答(恰好一次,
// 可在任意线程、可延后调用 channel_reply;不应答则 JS 侧在 handler 注销/引擎关闭时得到 null)。
typedef void (*MomoChannelHandlerFn)(void* user_data, const uint8_t* bytes, size_t len,
                                     uint64_t reply_token);

// ---------- JS 调用参数访问(MomoCallInfo, 均仅 ui 线程、回调栈内有效) ----------

typedef struct MomoCallApi {
  size_t struct_size;
  size_t (*arg_count)(MomoCallInfoRef);
  MomoStatus (*arg_double)(MomoCallInfoRef, size_t index, double* out);
  MomoStatus (*arg_bool)(MomoCallInfoRef, size_t index, bool* out);
  // 字符串以 UTF-8 返回: 两段式(先询问长度)。
  MomoStatus (*arg_string_utf8)(MomoCallInfoRef, size_t index, char* buf, size_t buf_len,
                                size_t* out_len);
  // ArrayBuffer/TypedArray 的只读视图(仅回调期间有效)。
  MomoStatus (*arg_bytes)(MomoCallInfoRef, size_t index, const uint8_t** out_ptr, size_t* out_len);
  void (*return_double)(MomoCallInfoRef, double value);
  void (*return_bool)(MomoCallInfoRef, bool value);
  void (*return_string_utf8)(MomoCallInfoRef, const char* str, size_t len);
  void (*return_bytes_copy)(MomoCallInfoRef, const uint8_t* bytes, size_t len);
  // 返回 Promise: 宿主发 promise_token, 扩展稍后在任意线程 resolve/reject(经 host.promise_*)。
  void (*return_promise)(MomoCallInfoRef, uint64_t* out_promise_token);
  void (*throw_error)(MomoCallInfoRef, const char* message_utf8);
  void (*return_null)(MomoCallInfoRef);
} MomoCallApi;

// ---------- 外部纹理(扩展向引擎合成管线注入图像内容) ----------

typedef enum MomoTextureKind {
  kMomoTexturePixelBuffer = 0,  // CPU 像素缓冲(RGBA8, 扩展填充)
  kMomoTextureMetalTexture = 1, // macOS: MTLTexture*
  kMomoTextureD3DSharedHandle = 2, // Windows: D3D11 共享句柄
} MomoTextureKind;

// v1 只实现 kMomoTexturePixelBuffer(GPU 共享句柄为预留, 注册时返回 kMomoErrInvalidArgs)。
// 两种推送方式(任选其一):
//   1) texture_update_pixels: 扩展直接推送一帧 RGBA8(非预乘)像素, 宿主拷贝。
//   2) desc.acquire/release: 扩展在**注册该纹理的线程上**调用 texture_mark_frame_available 时, 宿主在
//      该线程上同步回调 acquire 取像素(每像素 4 字节, 行紧密排列)、拷贝后回调 release。
//      在其他线程调用返回 kMomoErrWrongThread(回调只在注册线程执行)。
// 两种方式都不会在 raster 线程回调扩展代码, 因此 on_detach 之后不会再有纹理回调。
typedef struct MomoTextureDesc {
  size_t struct_size;
  MomoTextureKind kind;
  const void* (*acquire)(void* user_data, uint32_t* out_width, uint32_t* out_height);  // 可空
  void (*release)(void* user_data, const void* pixels);                               // 可空
  void* user_data;
} MomoTextureDesc;

// ---------- 平台视图(扩展提供系统原生视图, 由引擎嵌入 momo 画面, docs/05 §9) ----------

// 工厂回调全部在 platform 线程执行(原生视图只能在主线程创建/销毁)。
// create: 为 view_id 创建原生视图(macOS: NSView*;Windows: HWND), params 为 JS 传入的创建参数
//   (仅回调期间有效)。返回 NULL 表示失败。返回的视图归扩展所有: 宿主只借用该指针, 挂进窗口期间由
//   embedder 额外持有引用(macOS 为 strong 引用), 直到 dispose。
//   macOS 惯用法: return (void*)CFBridgingRetain(view);  dispose 中 CFBridgingRelease(native_view)。
// dispose: 宿主已把视图移出窗口;扩展在此释放它。每个成功的 create 恰好对应一次 dispose
//   (JS 销毁、热重启、引擎关闭时;on_detach 之前一定已全部 dispose)。
typedef void* (*MomoPlatformViewCreateFn)(void* user_data, int64_t view_id, const uint8_t* params,
                                          size_t params_len);
typedef void (*MomoPlatformViewDisposeFn)(void* user_data, int64_t view_id, void* native_view);

typedef struct MomoPlatformViewFactory {
  size_t struct_size;
  MomoPlatformViewCreateFn create;
  MomoPlatformViewDisposeFn dispose;
  void* user_data;
} MomoPlatformViewFactory;

// ---------- 宿主 API 函数表 ----------

typedef struct MomoHostApi {
  size_t struct_size;
  uint32_t api_version;  // == MOMO_EXT_API_VERSION

  // -- 模块与函数注册(on_attach 内调用; setup 在 ui 线程执行) --
  MomoStatus (*register_module)(MomoEngineRef, const char* specifier /* "ext:hello" */,
                                MomoModuleSetupFn setup, void* user_data);
  MomoStatus (*builder_add_function)(MomoJsBuilderRef, const char* name, MomoNativeFn fn,
                                     void* user_data);

  const MomoCallApi* call;  // JS 调用参数访问函数表

  // -- Promise(任意线程) --
  MomoStatus (*promise_resolve_bytes)(MomoEngineRef, uint64_t promise_token,
                                      const uint8_t* bytes, size_t len);
  MomoStatus (*promise_reject)(MomoEngineRef, uint64_t promise_token, const char* message_utf8);

  // -- 消息通道(注册任意线程; handler 在 runner 指定的线程上被调;handler 为 NULL = 注销) --
  MomoStatus (*channel_set_handler)(MomoEngineRef, const char* channel, MomoTaskRunner runner,
                                    MomoChannelHandlerFn handler, void* user_data);
  MomoStatus (*channel_reply)(MomoEngineRef, uint64_t reply_token, const uint8_t* bytes,
                              size_t len);
  // native → JS(任意线程, 不等待回复)。
  MomoStatus (*channel_send)(MomoEngineRef, const char* channel, const uint8_t* bytes, size_t len);

  // -- 任务投递(任意线程) --
  MomoStatus (*post_task)(MomoEngineRef, MomoTaskRunner runner, MomoWorkFn fn, void* user_data);
  MomoStatus (*post_delayed_task)(MomoEngineRef, MomoTaskRunner runner, uint64_t delay_micros,
                                  MomoWorkFn fn, void* user_data);

  // -- 窗口与平台(仅 platform 线程) --
  // 返回 HWND(Windows)/ NSView*(macOS)。view_id==0 为主窗口。
  MomoStatus (*get_window_handle)(MomoEngineRef, uint64_t view_id, void** out_handle);

  // -- 帧回调(仅在 ui 线程注册, 否则返回 kMomoErrWrongThread;回调在 ui 线程每 vsync 帧开始时调用,
  //    用于驱动动画类扩展;注册期间引擎持续出帧) --
  MomoStatus (*add_frame_callback)(MomoEngineRef, MomoWorkFn fn, void* user_data,
                                   uint64_t* out_callback_id);
  MomoStatus (*remove_frame_callback)(MomoEngineRef, uint64_t callback_id);

  // -- 外部纹理(注册任意线程;acquire/release 方式下 mark_frame_available 须在注册线程调用, 见上) --
  MomoStatus (*texture_register)(MomoEngineRef, const MomoTextureDesc* desc,
                                 int64_t* out_texture_id);
  MomoStatus (*texture_mark_frame_available)(MomoEngineRef, int64_t texture_id);
  MomoStatus (*texture_unregister)(MomoEngineRef, int64_t texture_id);
  // 推送一帧 RGBA8(非预乘)像素并标记可用(任意线程, 宿主拷贝)。
  MomoStatus (*texture_update_pixels)(MomoEngineRef, int64_t texture_id, const uint8_t* rgba,
                                      uint32_t width, uint32_t height, size_t row_bytes);

  // -- 资源与日志(任意线程) --
  MomoStatus (*open_asset)(MomoEngineRef, const char* path, const uint8_t** out_data,
                           size_t* out_len);  // 配对 free_asset
  void (*free_asset)(MomoEngineRef, const uint8_t* data);
  void (*log)(MomoLogLevel level, const char* tag, const char* message_utf8);

  // ==== 以下为 v1 追加字段(扩展用 struct_size 判断宿主是否提供) ====

  // -- 平台视图(仅 platform 线程;其他线程返回 kMomoErrWrongThread) --
  // 注册视图类型(反域名风格, 如 "acme.webview";"momo/" 前缀保留)。重复注册返回 kMomoErrInvalidArgs。
  // JS 侧以同名类型创建视图(框架 PlatformView widget / PlatformViewsService)。
  MomoStatus (*platform_view_register_factory)(MomoEngineRef, const char* view_type,
                                               const MomoPlatformViewFactory* factory);
  // 取已创建视图的原生指针(扩展在自己的通道 handler 里按 id 找到视图)。未知 id 返回 kMomoErrNotFound。
  MomoStatus (*platform_view_get)(MomoEngineRef, int64_t view_id, void** out_native_view);
} MomoHostApi;

// ---------- 扩展描述与入口 ----------

typedef struct MomoExtension {
  size_t struct_size;
  const char* id;  // 反域名风格, 如 "acme.sqlite", 与 manifest 一致
  void* user_data;
  // on_attach: platform 线程调用。host 与 engine 在 on_detach 前保持有效。
  void (*on_attach)(void* user_data, const MomoHostApi* host, MomoEngineRef engine);
  // on_detach: 引擎关闭时调用(platform 线程, JS 运行时已销毁)。必须停止自己的线程/定时任务,
  // 之后宿主忽略该扩展的一切 API 调用(返回 kMomoErrNotFound)。动态库不会被卸载(与 Flutter 插件一致),
  // 因此已投递但尚未执行的 post_task 回调仍然安全。
  void (*on_detach)(void* user_data);
} MomoExtension;

// 扩展动态库必须导出的唯一符号。host_api_version 为宿主支持的最高版本;
// 扩展不支持时返回 false(宿主将禁用该扩展并记录错误)。
typedef bool (*MomoExtInitFn)(uint32_t host_api_version, MomoExtension* out_extension);
// 实现方签名: MOMO_EXT_EXPORT bool momo_ext_init(uint32_t, MomoExtension*);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MOMO_EXTENSION_H_
