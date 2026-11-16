// momo_host.h — Momo 宿主能力表(稳定 C ABI, v1)。设计说明: docs/07-extension-system.md。
//
// 原生扩展是普通的 Node.js 原生插件(.node, Node-API): JS 与原生之间的一切——函数、对象、Promise、
// 从别的线程回到 JS——都用 Node-API。本头文件只描述 Node-API 给不了的东西, 即引擎自己的能力:
//   * 把任务投到 OS 主线程(platform 线程;原生 UI 只能在那里用);
//   * 外部纹理: 插件生成像素, 引擎合成进画面;
//   * 平台视图: 插件提供系统原生视图, 引擎负责嵌入、合成、输入与焦点。
//
// 取得宿主表: JS 侧 `import { nativeHost } from "momo:services"`(一个 BigInt: 宿主表的地址), 把它传给
// 插件的某个函数, 插件里用 momo_host_from_napi(env, value) 取出(见文件末尾;需要先包含 <node_api.h>)。
// 指针在进程存续期间一直有效(引擎关闭后各函数返回 kMomoErrClosed), 可以保存, 可以在任意线程使用。
//
// 约定:
//   * 纯 C、无异常、无 C++ 类型跨边界;
//   * MomoHost 只在末尾追加字段, 已有字段不改不删;插件用 struct_size 判断宿主是否提供某个函数;
//   * 宿主只在 platform 线程调用插件(平台任务、视图工厂), 而且只在引擎关闭之前——见 MomoHost 的"关闭"。

#ifndef MOMO_HOST_H_
#define MOMO_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOMO_HOST_API_VERSION 1u

typedef enum MomoStatus {
  kMomoOk = 0,
  kMomoErrInvalidArgs = 1,
  kMomoErrWrongThread = 2,
  kMomoErrNotFound = 3,
  kMomoErrClosed = 4,  // 引擎已关闭(或正在关闭)
} MomoStatus;

typedef void (*MomoTaskFn)(void* user_data);

// ---------- 平台视图(docs/05 §9) ----------

// 两个回调都在 platform 线程执行(原生视图只能在主线程创建/销毁)。
// create: 为 view_id 创建原生视图(macOS: NSView*;Windows: HWND), params 为 JS 传入的创建参数
//   (仅回调期间有效)。返回 NULL 表示失败。返回的视图归插件所有: 宿主只借用该指针, 挂进窗口期间由
//   embedder 额外持有引用, 直到 dispose。
//   macOS 惯用法: return (void*)CFBridgingRetain(view);  dispose 中 CFBridgingRelease(native_view)。
// dispose: 宿主已把视图移出窗口;插件在此释放它。每个成功的 create 恰好对应一次 dispose
//   (JS 销毁它、或引擎关闭时)。
typedef void* (*MomoPlatformViewCreateFn)(void* user_data, int64_t view_id, const uint8_t* params,
                                          size_t params_len);
typedef void (*MomoPlatformViewDisposeFn)(void* user_data, int64_t view_id, void* native_view);

typedef struct MomoPlatformViewFactory {
  size_t struct_size;
  MomoPlatformViewCreateFn create;
  MomoPlatformViewDisposeFn dispose;
  void* user_data;
} MomoPlatformViewFactory;

// ---------- 宿主能力表 ----------
//
// 关闭: 引擎关闭时(platform 线程)依次: ① 关闭本表——此后每个函数返回 kMomoErrClosed, 已投递而未执行的
// 平台任务被丢弃(不会执行);② dispose 全部平台视图;③ 销毁 JS 运行时, 插件用 napi_add_env_cleanup_hook
// 登记的清理函数在这一步执行。所以清理函数执行时, 宿主已不会再调用插件;插件在清理函数里停掉自己的线程。
typedef struct MomoHost {
  size_t struct_size;
  uint32_t api_version;  // == MOMO_HOST_API_VERSION

  // 在 platform 线程执行 fn(user_data)。任意线程调用;任务按投递顺序执行。
  MomoStatus (*post_platform_task)(const struct MomoHost*, MomoTaskFn fn, void* user_data);

  // -- 外部纹理(任意线程)。JS 侧用纹理 id 显示(框架的 Texture, 或 SceneBuilder.addTexture) --
  MomoStatus (*texture_create)(const struct MomoHost*, int64_t* out_texture_id);
  // 推送一帧 RGBA8(非预乘)像素: 宿主拷贝, 并请求引擎出一帧。
  MomoStatus (*texture_update_pixels)(const struct MomoHost*, int64_t texture_id,
                                      const uint8_t* rgba, uint32_t width, uint32_t height,
                                      size_t row_bytes);
  MomoStatus (*texture_destroy)(const struct MomoHost*, int64_t texture_id);

  // -- 平台视图 --
  // 注册视图类型(反域名风格, 如 "acme.webview";"momo/" 前缀保留;重复注册返回 kMomoErrInvalidArgs)。
  // 任意线程调用。JS 侧以同名类型创建视图(框架的 PlatformView)。类型一经注册, 到引擎关闭为止都有效。
  MomoStatus (*platform_view_register_factory)(const struct MomoHost*, const char* view_type,
                                               const MomoPlatformViewFactory* factory);
  // 取已创建视图的原生指针。仅 platform 线程;未知(或已 dispose 的)id 返回 kMomoErrNotFound——
  // 插件每次按 id 查, 不缓存指针。
  MomoStatus (*platform_view_get)(const struct MomoHost*, int64_t view_id, void** out_native_view);
} MomoHost;

// nativeHost(JS 值)→ 宿主表;不是合法的 nativeHost 时返回 NULL。先包含 <node_api.h> 才有这个函数。
#ifdef NAPI_VERSION
static inline const MomoHost* momo_host_from_napi(napi_env env, napi_value native_host) {
  uint64_t address = 0;
  bool lossless = false;
  if (napi_get_value_bigint_uint64(env, native_host, &address, &lossless) != napi_ok || !lossless) {
    return NULL;
  }
  return (const MomoHost*)(uintptr_t)address;
}
#endif

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MOMO_HOST_H_
