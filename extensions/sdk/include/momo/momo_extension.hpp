// momo_extension.hpp — Momo 原生扩展的 C++ 便利封装(header-only, C++17)
//
// 完全建立在 momo_extension.h 的稳定 C ABI 之上: 不引入任何新的跨边界符号, 扩展与宿主仍可用不同的
// 编译器 / 标准库构建。全部为 inline 代码, 本身不使用异常与 RTTI, 开不开 -fno-exceptions / -fno-rtti
// 都能编译。扩展自己的代码可以用异常, 但异常不得穿过 C ABI 边界: 注册给宿主的 lambda 里必须捕获。
// 请以隐藏可见性构建扩展(SDK 的 momo_add_extension() 已设置), 保证只导出 momo_ext_init。
//
// 用法:
//
//   #include "momo/momo_extension.hpp"
//
//   class Hello final : public momo::ext::Extension {
//    public:
//     void OnAttach(momo::ext::Host& host) override {
//       host.RegisterModule("ext:hello", [](momo::ext::ModuleBuilder& m) {
//         m.Function("add", [](momo::ext::Call& call) {
//           auto a = call.Number(0), b = call.Number(1);
//           if (!a || !b) return call.Throw("add(a, b) expects two numbers");
//           call.Return(*a + *b);
//         });
//       });
//     }
//   };
//   MOMO_EXTENSION(Hello, "acme.hello")
//
// 线程约定与 C ABI 完全相同(见 momo_extension.h 中每个函数的注释):
//   - OnAttach / OnDetach 在 platform 线程;
//   - 模块 setup 与 JS 函数在 ui 线程, 禁止阻塞;Call 只在回调栈内有效;
//   - 通道 handler 在注册时显式指定的 runner 上执行;PostTask 的任务在指定的 runner 上执行。
//
// 生命周期: 注册给宿主的 std::function(JS 函数、通道 handler、模块 setup)由 Host 持有, 有意不在
// on_detach 时释放——宿主不卸载扩展动态库, 在途的回调可能晚于 on_detach 返回, 释放它们会悬挂。
// PostTask 的任务对象在执行后释放;引擎关闭时被丢弃的任务会泄漏其闭包(进程即将退出)。

#ifndef MOMO_EXTENSION_HPP_
#define MOMO_EXTENSION_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "momo/momo_extension.h"

namespace momo::ext {

// 只读字节视图(仅在回调期间有效, 需要保留必须拷贝)。
struct Bytes {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// 一次 JS 调用(仅 ui 线程、回调栈内有效)。
class Call {
 public:
  Call(const MomoCallApi* api, MomoCallInfoRef info) : api_(api), info_(info) {}

  size_t size() const { return api_->arg_count(info_); }

  std::optional<double> Number(size_t index) const {
    double value = 0;
    if (api_->arg_double(info_, index, &value) != kMomoOk) return std::nullopt;
    return value;
  }
  std::optional<bool> Bool(size_t index) const {
    bool value = false;
    if (api_->arg_bool(info_, index, &value) != kMomoOk) return std::nullopt;
    return value;
  }
  std::optional<std::string> String(size_t index) const {
    size_t len = 0;
    if (api_->arg_string_utf8(info_, index, nullptr, 0, &len) != kMomoOk) return std::nullopt;
    std::string out(len, '\0');
    if (api_->arg_string_utf8(info_, index, out.data(), len + 1, &len) != kMomoOk) {
      return std::nullopt;
    }
    out.resize(len);
    return out;
  }
  // ArrayBuffer / TypedArray 的只读视图。
  std::optional<Bytes> ByteView(size_t index) const {
    Bytes out;
    if (api_->arg_bytes(info_, index, &out.data, &out.size) != kMomoOk) return std::nullopt;
    return out;
  }

  void Return(double value) { api_->return_double(info_, value); }
  void Return(bool value) { api_->return_bool(info_, value); }
  void Return(std::string_view utf8) { api_->return_string_utf8(info_, utf8.data(), utf8.size()); }
  void Return(const char* utf8) { Return(std::string_view(utf8)); }
  void ReturnBytes(const void* data, size_t size) {
    api_->return_bytes_copy(info_, static_cast<const uint8_t*>(data), size);
  }
  void ReturnNull() { api_->return_null(info_); }
  // 返回一个 Promise;稍后用 Host::Resolve / Host::Reject 在任意线程结算。
  uint64_t ReturnPromise() {
    uint64_t token = 0;
    api_->return_promise(info_, &token);
    return token;
  }
  void Throw(const char* message_utf8) { api_->throw_error(info_, message_utf8); }

  MomoCallInfoRef raw() const { return info_; }

 private:
  const MomoCallApi* api_;
  MomoCallInfoRef info_;
};

using JsFunction = std::function<void(Call& call)>;
// 通道 handler: bytes 仅在回调期间有效;reply_token 交给 Host::Reply(恰好一次, 可延后、可跨线程)。
using ChannelHandler = std::function<void(Bytes bytes, uint64_t reply_token)>;

class Host;

// 模块构建器(仅在 RegisterModule 的 setup 回调内有效, ui 线程)。
class ModuleBuilder {
 public:
  ModuleBuilder(Host& host, MomoJsBuilderRef builder) : host_(host), builder_(builder) {}
  MomoStatus Function(const char* name, JsFunction fn);

 private:
  Host& host_;
  MomoJsBuilderRef builder_;
};

// 宿主能力(on_attach 到 on_detach 之间有效)。方法的线程约定见 momo_extension.h。
class Host {
 public:
  Host(const MomoHostApi* api, MomoEngineRef engine) : api_(api), engine_(engine) {}
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  const MomoHostApi* api() const { return api_; }
  MomoEngineRef engine() const { return engine_; }

  // ---- 模块(仅 OnAttach 内调用;setup 在 ui 线程执行) ----
  MomoStatus RegisterModule(const char* specifier, std::function<void(ModuleBuilder&)> setup) {
    auto* holder = Keep(std::make_unique<SetupHolder>(SetupHolder{this, std::move(setup)}));
    return api_->register_module(engine_, specifier, &SetupTrampoline, holder);
  }

  // ---- 消息通道 ----
  MomoStatus SetChannelHandler(const char* channel, MomoTaskRunner runner, ChannelHandler handler) {
    if (!handler) return api_->channel_set_handler(engine_, channel, runner, nullptr, nullptr);
    auto* holder = Keep(std::make_unique<ChannelHandler>(std::move(handler)));
    return api_->channel_set_handler(engine_, channel, runner, &ChannelTrampoline, holder);
  }
  MomoStatus Reply(uint64_t reply_token, const void* data, size_t size) {
    return api_->channel_reply(engine_, reply_token, static_cast<const uint8_t*>(data), size);
  }
  MomoStatus Send(const char* channel, const void* data, size_t size) {
    return api_->channel_send(engine_, channel, static_cast<const uint8_t*>(data), size);
  }

  // ---- Promise(任意线程) ----
  MomoStatus Resolve(uint64_t promise_token, const void* data, size_t size) {
    return api_->promise_resolve_bytes(engine_, promise_token, static_cast<const uint8_t*>(data),
                                       size);
  }
  MomoStatus Reject(uint64_t promise_token, const char* message_utf8) {
    return api_->promise_reject(engine_, promise_token, message_utf8);
  }

  // ---- 任务投递(任意线程;task 在 runner 指定的线程执行一次后释放) ----
  MomoStatus PostTask(MomoTaskRunner runner, std::function<void()> task) {
    auto* boxed = new std::function<void()>(std::move(task));
    const MomoStatus status = api_->post_task(engine_, runner, &TaskTrampoline, boxed);
    if (status != kMomoOk) delete boxed;
    return status;
  }
  MomoStatus PostDelayedTask(MomoTaskRunner runner, uint64_t delay_micros,
                             std::function<void()> task) {
    auto* boxed = new std::function<void()>(std::move(task));
    const MomoStatus status =
        api_->post_delayed_task(engine_, runner, delay_micros, &TaskTrampoline, boxed);
    if (status != kMomoOk) delete boxed;
    return status;
  }

  // ---- 帧回调(ui 线程, 每 vsync;注册期间引擎持续出帧) ----
  // 返回回调 id(失败为 0)。
  uint64_t AddFrameCallback(std::function<void()> callback) {
    auto* holder = Keep(std::make_unique<std::function<void()>>(std::move(callback)));
    uint64_t id = 0;
    if (api_->add_frame_callback(engine_, &FrameTrampoline, holder, &id) != kMomoOk) return 0;
    return id;
  }
  MomoStatus RemoveFrameCallback(uint64_t id) { return api_->remove_frame_callback(engine_, id); }

  // ---- 外部纹理 ----
  // 注册一个 CPU 像素纹理(用 UpdatePixels 推帧);返回纹理 id(失败为 0)。
  int64_t RegisterPixelTexture() {
    int64_t id = 0;
    if (api_->texture_register(engine_, nullptr, &id) != kMomoOk) return 0;
    return id;
  }
  MomoStatus UpdatePixels(int64_t texture_id, const uint8_t* rgba, uint32_t width,
                          uint32_t height, size_t row_bytes) {
    return api_->texture_update_pixels(engine_, texture_id, rgba, width, height, row_bytes);
  }
  MomoStatus UnregisterTexture(int64_t texture_id) {
    return api_->texture_unregister(engine_, texture_id);
  }

  // ---- 平台(仅 platform 线程) ----
  void* MainWindowHandle() {
    void* handle = nullptr;
    return api_->get_window_handle(engine_, 0, &handle) == kMomoOk ? handle : nullptr;
  }

  // ---- 平台视图(仅 platform 线程;宿主不支持时返回 kMomoErrNotFound / nullptr) ----
  bool SupportsPlatformViews() const {
    return api_->struct_size >= offsetof(MomoHostApi, platform_view_get) + sizeof(void*);
  }
  // create 返回原生视图(NSView* / HWND, 所有权归扩展, 见 momo_extension.h);dispose 释放它。
  using PlatformViewCreate = std::function<void*(int64_t view_id, Bytes params)>;
  using PlatformViewDispose = std::function<void(int64_t view_id, void* native_view)>;
  MomoStatus RegisterPlatformViewFactory(const char* view_type, PlatformViewCreate create,
                                         PlatformViewDispose dispose) {
    if (!SupportsPlatformViews()) return kMomoErrNotFound;
    auto* holder = Keep(std::make_unique<ViewFactoryHolder>(
        ViewFactoryHolder{std::move(create), std::move(dispose)}));
    MomoPlatformViewFactory factory{sizeof(MomoPlatformViewFactory), &ViewCreateTrampoline,
                                    &ViewDisposeTrampoline, holder};
    return api_->platform_view_register_factory(engine_, view_type, &factory);
  }
  void* PlatformView(int64_t view_id) {
    void* view = nullptr;
    if (!SupportsPlatformViews()) return nullptr;
    return api_->platform_view_get(engine_, view_id, &view) == kMomoOk ? view : nullptr;
  }

  // ---- 资源与日志(任意线程) ----
  // 读取打包进应用的资源;未找到返回 std::nullopt。
  std::optional<std::vector<uint8_t>> ReadAsset(const char* path) {
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (api_->open_asset(engine_, path, &data, &size) != kMomoOk) return std::nullopt;
    std::vector<uint8_t> out(data, data + size);
    api_->free_asset(engine_, data);
    return out;
  }
  void Log(MomoLogLevel level, const char* tag, const char* message_utf8) {
    api_->log(level, tag, message_utf8);
  }

 private:
  friend class ModuleBuilder;

  struct SetupHolder {
    Host* host;
    std::function<void(ModuleBuilder&)> setup;
  };

  // 持有注册给宿主的回调对象(见文件头"生命周期")。register/setup 可能来自不同线程, 加锁。
  template <typename T>
  T* Keep(std::unique_ptr<T> object) {
    T* raw = object.get();
    std::lock_guard<std::mutex> lock(mutex_);
    kept_.emplace_back(raw, [](void* p) { delete static_cast<T*>(p); });
    object.release();
    return raw;
  }

  static void SetupTrampoline(void* user_data, MomoJsBuilderRef builder) {
    auto* holder = static_cast<SetupHolder*>(user_data);
    ModuleBuilder module(*holder->host, builder);
    holder->setup(module);
  }
  static void FunctionTrampoline(void* user_data, MomoCallInfoRef info);
  static void ChannelTrampoline(void* user_data, const uint8_t* bytes, size_t len,
                                uint64_t reply_token) {
    (*static_cast<ChannelHandler*>(user_data))(Bytes{bytes, len}, reply_token);
  }
  static void TaskTrampoline(void* user_data) {
    std::unique_ptr<std::function<void()>> task(static_cast<std::function<void()>*>(user_data));
    (*task)();
  }
  static void FrameTrampoline(void* user_data) {
    (*static_cast<std::function<void()>*>(user_data))();
  }
  struct ViewFactoryHolder {
    PlatformViewCreate create;
    PlatformViewDispose dispose;
  };
  static void* ViewCreateTrampoline(void* user_data, int64_t view_id, const uint8_t* params,
                                    size_t len) {
    return static_cast<ViewFactoryHolder*>(user_data)->create(view_id, Bytes{params, len});
  }
  static void ViewDisposeTrampoline(void* user_data, int64_t view_id, void* native_view) {
    static_cast<ViewFactoryHolder*>(user_data)->dispose(view_id, native_view);
  }

  const MomoHostApi* api_;
  MomoEngineRef engine_;
  std::mutex mutex_;
  std::vector<std::unique_ptr<void, void (*)(void*)>> kept_;
};

namespace internal {
struct FunctionHolder {
  const MomoHostApi* api;
  JsFunction fn;
};
}  // namespace internal

inline MomoStatus ModuleBuilder::Function(const char* name, JsFunction fn) {
  auto* holder = host_.Keep(std::make_unique<internal::FunctionHolder>(
      internal::FunctionHolder{host_.api(), std::move(fn)}));
  return host_.api()->builder_add_function(builder_, name, &Host::FunctionTrampoline, holder);
}

inline void Host::FunctionTrampoline(void* user_data, MomoCallInfoRef info) {
  auto* holder = static_cast<internal::FunctionHolder*>(user_data);
  Call call(holder->api->call, info);
  holder->fn(call);
}

// 扩展基类。由 MOMO_EXTENSION 宏创建实例;一个实例对应一次 attach。
class Extension {
 public:
  virtual ~Extension() = default;
  // platform 线程。在这里注册模块、通道等。host 在 OnDetach 返回前有效。
  virtual void OnAttach(Host& host) = 0;
  // platform 线程, JS 运行时已销毁。必须停止自己的线程与定时任务;之后宿主忽略该扩展的一切调用。
  virtual void OnDetach() {}
};

namespace internal {

template <typename T>
struct Instance {
  std::unique_ptr<Host> host;
  T extension;
};

template <typename T>
bool Init(uint32_t host_api_version, MomoExtension* out, const char* id) {
  if (host_api_version < MOMO_EXT_API_VERSION || out == nullptr) return false;
  out->id = id;
  // 有意不释放(见文件头"生命周期"):宿主不卸载动态库, 在途回调可能晚于 on_detach。
  out->user_data = new Instance<T>();
  out->on_attach = [](void* user_data, const MomoHostApi* api, MomoEngineRef engine) {
    auto* instance = static_cast<Instance<T>*>(user_data);
    instance->host = std::make_unique<Host>(api, engine);
    instance->extension.OnAttach(*instance->host);
  };
  out->on_detach = [](void* user_data) {
    static_cast<Instance<T>*>(user_data)->extension.OnDetach();
  };
  return true;
}

}  // namespace internal
}  // namespace momo::ext

// 定义扩展入口 momo_ext_init。Class 须可默认构造并继承 momo::ext::Extension;id 与 manifest 一致。
#define MOMO_EXTENSION(Class, id)                                                          \
  extern "C" MOMO_EXT_EXPORT bool momo_ext_init(uint32_t host_api_version,               \
                                                MomoExtension* out) {                    \
    return ::momo::ext::internal::Init<Class>(host_api_version, out, id);                \
  }

#endif  // MOMO_EXTENSION_HPP_
