#pragma once

// ExtensionHost: 三方原生扩展的宿主(docs/07 §5)。实现稳定 C ABI(extensions/sdk/include/momo/
// momo_extension.h)中 MomoHostApi 的非 JS 部分;JS 模块(ext:*)的 V8 胶水在 engine/ui/extension_bindings.cc。
//
// 加载: 静态声明式——只加载 <extensions_dir>/extensions.manifest 中列出、且位于该目录内的动态库
// (不支持运行期从任意路径加载)。逐个 dlopen → 解析 momo_ext_init → 版本握手 → on_attach(platform 线程)。
// 单个扩展失败只禁用它并记录错误(errors(), 经 momo/extensions 通道报给 JS), 不影响引擎。
// 关闭: DetachAll(platform 线程, JS 运行时已销毁)→ 各扩展 on_detach → 之后其 API 调用一律返回
// kMomoErrNotFound。动态库永不卸载(扩展线程上可能仍有在途回调), 每个扩展的 MomoEngine 句柄也
// 有意保留到进程结束(几十字节), 保证 on_detach 之后的误调用不会访问已释放内存。
//
// 线程约定: 创建/加载/DetachAll 在 platform 线程(momo::Object 归属);其余公开方法线程安全
// (内部小临界区, 回调不在锁内执行)。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/lock.h"
#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "base/worker_pool.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/platform_view_registry.h"
#include "engine/common/task_runners.h"
#include "engine/graphics/texture.h"
#include "momo/momo_extension.h"

namespace momo::engine {

struct ExtensionManifestEntry {
  std::string id;          // "acme.sqlite"
  std::string library;     // 相对 extensions_dir 的动态库路径
  std::string specifier;   // "ext:sqlite"(信息性, 模块由扩展在 on_attach 中注册)
  std::vector<std::string> capabilities;
};

// manifest 格式: {"extensions": [{"id": ..., "library": ..., "specifier": ..., "capabilities": [...]}]}
StatusOr<std::vector<ExtensionManifestEntry>> ParseExtensionManifest(std::string_view json);

inline constexpr char kExtensionManifestName[] = "extensions.manifest";

class ExtensionHost final : public Object {
 public:
  struct Environment {
    TaskRunners runners;
    std::shared_ptr<MessageChannelHub> hub;
    std::shared_ptr<graphics::TextureRegistry> textures;
    std::filesystem::path extensions_dir;
    std::filesystem::path assets_dir;
    // platform 线程上调用: 视图(窗口)的原生句柄(NSView* / HWND), 视图不存在为空。
    std::function<void*(int64_t view_id)> window_handle;
    // 请求引擎出一帧(帧回调注册时)。总是被投递到 runners.platform 上执行(注册方线程),
    // 不会在扩展调用宿主 API 的线程上直接调用。
    std::function<void()> request_frame;
    // 平台视图表(platform 线程;可空 = 不支持平台视图)。由 Shell 持有并负责实例的创建/销毁。
    std::shared_ptr<PlatformViewRegistry> platform_views;
  };

  // JS 模块注册(扩展在 on_attach 中调用 register_module)。setup 在 ui 线程执行。
  struct ModuleRegistration {
    std::string specifier;
    MomoModuleSetupFn setup = nullptr;
    void* user_data = nullptr;
    MomoEngineRef engine = nullptr;
    std::string extension_id;
  };

  // ui 线程上的 Promise 结算端(由 ext:* 的 JS 胶水设置)。error 非空 = reject。
  using PromiseSink = std::function<void(uint64_t token, std::optional<std::vector<uint8_t>> bytes,
                                         const std::string& error)>;

  explicit ExtensionHost(Environment env);  // platform 线程
  ~ExtensionHost() override;                // 隐式 DetachAll

  // 读取 manifest 并加载其中的扩展;manifest 不存在 = 无扩展(返回 Ok)。
  // 返回值只反映 manifest 本身的错误;单个扩展的失败记入 errors()。
  Status LoadFromManifest();
  // 加载进程内(静态链接/测试)扩展。
  Status AddExtension(const std::string& expected_id, MomoExtInitFn init);
  // 关闭: on_detach 全部扩展, 停止 worker pool。幂等。
  void DetachAll();

  std::vector<std::string> attached_ids() const;
  std::vector<std::string> errors() const;
  std::vector<ModuleRegistration> modules() const;

  // ---- ui 线程侧 ----
  // 每帧开始时调用(ui 线程)。返回是否仍有已注册的帧回调(调用方据此继续请求帧)。
  bool RunFrameCallbacks();
  bool has_frame_callbacks() const;
  void SetPromiseSink(PromiseSink sink);
  // JS 胶水(V8)提供的函数: 调用参数访问表与 builder_add_function。在 ui 线程、安装 ext:* 模块之前设置;
  // 未设置时为返回错误的桩(无 JS 运行时的构建)。
  using AddFunctionFn = MomoStatus (*)(MomoJsBuilderRef, const char*, MomoNativeFn, void*);
  void SetJsGlue(const MomoCallApi* call_api, AddFunctionFn add_function);
  // 本宿主的函数表(on_attach 收到的同一个指针)。
  const MomoHostApi* host_api() const;

  struct Core;  // 实现细节(extension_host.cc)

 private:
  Status Attach(const std::string& expected_id, MomoExtInitFn init, std::string origin);

  std::shared_ptr<Core> core_;
  bool detached_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(ExtensionHost);
};

}  // namespace momo::engine
