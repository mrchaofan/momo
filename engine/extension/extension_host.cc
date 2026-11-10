#include "engine/extension/extension_host.h"

#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include "base/logging.h"
#include "engine/assets/asset_manager.h"
#include "engine/common/json.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// MomoEngine: C ABI 中不透明的引擎句柄, 每个扩展一个(便于识别调用方与 on_detach 后拒绝调用)。
struct MomoEngine {
  std::shared_ptr<momo::engine::ExtensionHost::Core> core;
  std::string id;
};

// MomoJsBuilder 由 V8 胶水定义(engine/ui/extension_bindings.cc);本文件只转发。

namespace momo::engine {

namespace {

constexpr char kTag[] = "momo.ext";

}  // namespace

struct ExtensionHost::Core {
  explicit Core(Environment e) : env(std::move(e)) {}

  Environment env;
  MomoHostApi api{};

  mutable Mutex mutex;
  bool alive = true;
  std::unique_ptr<WorkerPool> pool;  // 首次使用时创建
  std::vector<ModuleRegistration> modules;
  struct FrameCallback {
    MomoWorkFn fn;
    void* user_data;
    std::string extension_id;
  };
  std::map<uint64_t, FrameCallback> frame_callbacks;
  uint64_t next_frame_callback = 1;
  std::map<uint64_t, MessageReply> replies;  // channel reply_token → 回复句柄
  uint64_t next_reply = 1;
  std::vector<std::string> channels;         // 扩展注册过的通道(detach 时注销)
  // acquire/release 方式的纹理: 描述 + 注册线程(回调只在注册线程上执行)。
  struct TextureSource {
    MomoTextureDesc desc;
    std::thread::id owner;
  };
  std::map<int64_t, TextureSource> texture_sources;
  PromiseSink promise_sink;                  // ui 线程使用
  struct Attached {
    std::string id;
    MomoExtension extension;
    MomoEngine* engine;
    bool detached = false;
  };
  std::vector<Attached> attached;
  std::vector<std::string> errors;

  bool IsAlive() const {
    MutexLock lock(mutex);
    return alive;
  }
  std::shared_ptr<TaskRunner> Runner(MomoTaskRunner kind) const {
    switch (kind) {
      case kMomoRunnerPlatform: return env.runners.platform;
      case kMomoRunnerUi: return env.runners.ui;
      case kMomoRunnerIo: return env.runners.io;
      case kMomoRunnerWorkerPool: return nullptr;
    }
    return nullptr;
  }
  // 在指定线程执行 task。worker pool 已关闭/未知线程返回 false。
  bool Post(MomoTaskRunner kind, Closure task, TimeDelta delay) {
    if (kind == kMomoRunnerWorkerPool) {
      MutexLock lock(mutex);
      if (!alive) {
        return false;
      }
      if (pool == nullptr) {
        pool = std::make_unique<WorkerPool>("momo.ext", 2);
      }
      return delay > TimeDelta::zero() ? pool->PostDelayedTask(std::move(task), delay)
                                       : pool->PostTask(std::move(task));
    }
    std::shared_ptr<TaskRunner> runner = Runner(kind);
    if (runner == nullptr) {
      return false;
    }
    if (delay > TimeDelta::zero()) {
      runner->PostDelayedTask(std::move(task), delay);
    } else {
      runner->PostTask(std::move(task));
    }
    return true;
  }
};

namespace {

using Core = ExtensionHost::Core;

// 句柄 → Core(已 detach 返回空)。
Core* Live(MomoEngineRef engine) {
  if (engine == nullptr || engine->core == nullptr || !engine->core->IsAlive()) {
    return nullptr;
  }
  return engine->core.get();
}

// ---- 未安装 V8 胶水时的 MomoCallApi 桩 ----
size_t StubArgCount(MomoCallInfoRef) { return 0; }
MomoStatus StubArgDouble(MomoCallInfoRef, size_t, double*) { return kMomoErrInternal; }
MomoStatus StubArgBool(MomoCallInfoRef, size_t, bool*) { return kMomoErrInternal; }
MomoStatus StubArgString(MomoCallInfoRef, size_t, char*, size_t, size_t*) { return kMomoErrInternal; }
MomoStatus StubArgBytes(MomoCallInfoRef, size_t, const uint8_t**, size_t*) { return kMomoErrInternal; }
void StubReturnDouble(MomoCallInfoRef, double) {}
void StubReturnBool(MomoCallInfoRef, bool) {}
void StubReturnString(MomoCallInfoRef, const char*, size_t) {}
void StubReturnBytes(MomoCallInfoRef, const uint8_t*, size_t) {}
void StubReturnPromise(MomoCallInfoRef, uint64_t* token) { *token = 0; }
void StubThrow(MomoCallInfoRef, const char*) {}
void StubReturnNull(MomoCallInfoRef) {}

const MomoCallApi kStubCallApi = {
    sizeof(MomoCallApi), StubArgCount,     StubArgDouble,    StubArgBool,
    StubArgString,       StubArgBytes,     StubReturnDouble, StubReturnBool,
    StubReturnString,    StubReturnBytes,  StubReturnPromise, StubThrow,
    StubReturnNull,
};

// ---- MomoHostApi 实现 ----

MomoStatus RegisterModule(MomoEngineRef engine, const char* specifier, MomoModuleSetupFn setup,
                          void* user_data) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (specifier == nullptr || setup == nullptr) return kMomoErrInvalidArgs;
  const std::string spec(specifier);
  if (spec.rfind("ext:", 0) != 0 || spec.size() <= 4) {
    MOMO_LOG(ERROR) << "Extension " << engine->id << ": module specifier must be 'ext:<name>', got '"
                    << spec << "'";
    return kMomoErrInvalidArgs;
  }
  MutexLock lock(core->mutex);
  for (const auto& m : core->modules) {
    if (m.specifier == spec) {
      return kMomoErrInvalidArgs;  // 重复注册
    }
  }
  core->modules.push_back({spec, setup, user_data, engine, engine->id});
  return kMomoOk;
}

MomoStatus PromiseSettle(MomoEngineRef engine, uint64_t token,
                         std::optional<std::vector<uint8_t>> bytes, std::string error) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (token == 0) return kMomoErrInvalidArgs;
  std::shared_ptr<Core> keep = engine->core;
  core->env.runners.ui->PostTask(
      [keep, token, bytes = std::move(bytes), error = std::move(error)]() mutable {
        ExtensionHost::PromiseSink sink;
        {
          MutexLock lock(keep->mutex);
          sink = keep->promise_sink;
        }
        if (sink) {
          sink(token, std::move(bytes), error);
        }
      });
  return kMomoOk;
}

MomoStatus PromiseResolveBytes(MomoEngineRef engine, uint64_t token, const uint8_t* bytes,
                               size_t len) {
  if (bytes == nullptr && len != 0) return kMomoErrInvalidArgs;
  std::vector<uint8_t> data(bytes, bytes + len);
  return PromiseSettle(engine, token, std::move(data), {});
}

MomoStatus PromiseReject(MomoEngineRef engine, uint64_t token, const char* message) {
  return PromiseSettle(engine, token, std::nullopt,
                       message != nullptr && *message ? message : "rejected");
}

MomoStatus ChannelSetHandler(MomoEngineRef engine, const char* channel, MomoTaskRunner runner,
                             MomoChannelHandlerFn handler, void* user_data) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (channel == nullptr || *channel == '\0') return kMomoErrInvalidArgs;
  if (std::string_view(channel).rfind("momo/", 0) == 0) {
    return kMomoErrInvalidArgs;  // momo/* 为引擎保留
  }
  const std::string name(channel);
  if (handler == nullptr) {
    core->env.hub->SetNativeHandler(name, nullptr, nullptr);
    return kMomoOk;
  }
  std::shared_ptr<TaskRunner> target = core->Runner(runner);
  std::shared_ptr<Core> keep = engine->core;
  if (target == nullptr) {
    if (runner != kMomoRunnerWorkerPool) return kMomoErrInvalidArgs;
    target = core->env.runners.platform;  // 先到 platform, 再转投 worker pool
  }
  {
    MutexLock lock(core->mutex);
    core->channels.push_back(name);
  }
  core->env.hub->SetNativeHandler(
      name, target,
      [keep, handler, user_data, runner](MessageBytes message, MessageReply reply) {
        uint64_t token = 0;
        {
          MutexLock lock(keep->mutex);
          if (!keep->alive) {
            return;  // reply 析构 → null
          }
          token = keep->next_reply++;
          keep->replies.emplace(token, std::move(reply));
        }
        auto call = [keep, handler, user_data, token, message = std::move(message)]() {
          if (keep->IsAlive()) {
            handler(user_data, message.data(), message.size(), token);
          }
        };
        if (runner == kMomoRunnerWorkerPool) {
          keep->Post(kMomoRunnerWorkerPool, std::move(call), TimeDelta::zero());
        } else {
          call();
        }
      });
  return kMomoOk;
}

MomoStatus ChannelReply(MomoEngineRef engine, uint64_t token, const uint8_t* bytes, size_t len) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (bytes == nullptr && len != 0) return kMomoErrInvalidArgs;
  MessageReply reply;
  {
    MutexLock lock(core->mutex);
    auto it = core->replies.find(token);
    if (it == core->replies.end()) return kMomoErrNotFound;
    reply = std::move(it->second);
    core->replies.erase(it);
  }
  reply.Send(MessageBytes(bytes, bytes + len));
  return kMomoOk;
}

MomoStatus ChannelSend(MomoEngineRef engine, const char* channel, const uint8_t* bytes,
                       size_t len) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (channel == nullptr || (bytes == nullptr && len != 0)) return kMomoErrInvalidArgs;
  core->env.hub->SendToJs(channel, MessageBytes(bytes, bytes + len));
  return kMomoOk;
}

MomoStatus PostDelayedTask(MomoEngineRef engine, MomoTaskRunner runner, uint64_t delay_micros,
                           MomoWorkFn fn, void* user_data) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (fn == nullptr) return kMomoErrInvalidArgs;
  const bool ok = core->Post(
      runner, [fn, user_data] { fn(user_data); },
      std::chrono::microseconds(static_cast<int64_t>(delay_micros)));
  return ok ? kMomoOk : kMomoErrInvalidArgs;
}

MomoStatus PostTask(MomoEngineRef engine, MomoTaskRunner runner, MomoWorkFn fn, void* user_data) {
  return PostDelayedTask(engine, runner, 0, fn, user_data);
}

MomoStatus GetWindowHandle(MomoEngineRef engine, uint64_t view_id, void** out) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (out == nullptr) return kMomoErrInvalidArgs;
  if (!core->env.runners.platform->RunsTasksOnCurrentThread()) return kMomoErrWrongThread;
  if (!core->env.window_handle) return kMomoErrNotFound;
  *out = core->env.window_handle(static_cast<int64_t>(view_id));
  return *out != nullptr ? kMomoOk : kMomoErrNotFound;
}

MomoStatus AddFrameCallback(MomoEngineRef engine, MomoWorkFn fn, void* user_data, uint64_t* out_id) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (fn == nullptr || out_id == nullptr) return kMomoErrInvalidArgs;
  // 帧回调在 ui 线程执行, 因此只接受在 ui 线程上的注册(回调在调用方线程执行)。
  if (core->env.runners.ui == nullptr || !core->env.runners.ui->RunsTasksOnCurrentThread()) {
    return kMomoErrWrongThread;
  }
  {
    MutexLock lock(core->mutex);
    *out_id = core->next_frame_callback++;
    core->frame_callbacks[*out_id] = {fn, user_data, engine->id};
  }
  if (core->env.request_frame && core->env.runners.platform) {
    core->env.runners.platform->PostTask(core->env.request_frame);
  }
  return kMomoOk;
}

MomoStatus RemoveFrameCallback(MomoEngineRef engine, uint64_t id) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  MutexLock lock(core->mutex);
  return core->frame_callbacks.erase(id) ? kMomoOk : kMomoErrNotFound;
}

MomoStatus TextureRegister(MomoEngineRef engine, const MomoTextureDesc* desc, int64_t* out_id) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (out_id == nullptr || core->env.textures == nullptr) return kMomoErrInvalidArgs;
  if (desc != nullptr && (desc->struct_size < sizeof(size_t) + sizeof(MomoTextureKind) ||
                          desc->kind != kMomoTexturePixelBuffer)) {
    return kMomoErrInvalidArgs;  // v1 只支持 CPU 像素
  }
  std::shared_ptr<graphics::Texture> texture = core->env.textures->Register();
  *out_id = texture->id();
  if (desc != nullptr && desc->acquire != nullptr) {
    MutexLock lock(core->mutex);
    core->texture_sources[*out_id] = {*desc, std::this_thread::get_id()};
  }
  return kMomoOk;
}

MomoStatus TextureUpdatePixels(MomoEngineRef engine, int64_t id, const uint8_t* rgba,
                               uint32_t width, uint32_t height, size_t row_bytes) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  std::shared_ptr<graphics::Texture> texture =
      core->env.textures ? core->env.textures->Get(id) : nullptr;
  if (texture == nullptr) return kMomoErrNotFound;
  if (rgba == nullptr || width == 0 || height == 0) return kMomoErrInvalidArgs;
  if (!texture->UpdateRgba(rgba, static_cast<int>(width), static_cast<int>(height), row_bytes)
           .ok()) {
    return kMomoErrInvalidArgs;
  }
  core->env.textures->MarkFrameAvailable(id);
  return kMomoOk;
}

MomoStatus TextureMarkFrameAvailable(MomoEngineRef engine, int64_t id) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  std::optional<MomoTextureDesc> source;
  {
    MutexLock lock(core->mutex);
    auto it = core->texture_sources.find(id);
    if (it != core->texture_sources.end()) {
      // 回调(acquire/release)必须在注册方线程执行: 只接受在注册线程上发起的通知。
      if (it->second.owner != std::this_thread::get_id()) return kMomoErrWrongThread;
      source = it->second.desc;
    }
  }
  if (source) {
    // acquire/release 方式: 在调用线程(= 注册线程)上同步取帧并拷贝(不在 raster 线程回调扩展)。
    uint32_t w = 0, h = 0;
    const void* pixels = source->acquire(source->user_data, &w, &h);
    MomoStatus status = kMomoErrInvalidArgs;
    if (pixels != nullptr) {
      status = TextureUpdatePixels(engine, id, static_cast<const uint8_t*>(pixels), w, h,
                                   static_cast<size_t>(w) * 4);
    }
    if (source->release != nullptr) {
      source->release(source->user_data, pixels);
    }
    return status;
  }
  if (core->env.textures == nullptr || core->env.textures->Get(id) == nullptr) {
    return kMomoErrNotFound;
  }
  core->env.textures->MarkFrameAvailable(id);
  return kMomoOk;
}

MomoStatus TextureUnregister(MomoEngineRef engine, int64_t id) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  {
    MutexLock lock(core->mutex);
    core->texture_sources.erase(id);
  }
  if (core->env.textures) core->env.textures->Unregister(id);
  return kMomoOk;
}

MomoStatus OpenAsset(MomoEngineRef engine, const char* path, const uint8_t** out_data,
                     size_t* out_len) {
  Core* core = Live(engine);
  if (core == nullptr) return kMomoErrNotFound;
  if (path == nullptr || out_data == nullptr || out_len == nullptr) return kMomoErrInvalidArgs;
  StatusOr<std::vector<uint8_t>> data = assets::AssetManager(core->env.assets_dir).Load(path);
  if (!data.ok()) return kMomoErrNotFound;
  auto* copy = new uint8_t[data->empty() ? 1 : data->size()];  // free_asset 释放(跨 ABI 的唯一分配)
  std::copy(data->begin(), data->end(), copy);
  *out_data = copy;
  *out_len = data->size();
  return kMomoOk;
}

void FreeAsset(MomoEngineRef, const uint8_t* data) { delete[] data; }

void Log(MomoLogLevel level, const char* tag, const char* message) {
  const std::string text = std::string("[") + (tag ? tag : kTag) + "] " + (message ? message : "");
  switch (level) {
    case kMomoLogDebug: MOMO_LOG(VERBOSE) << text; break;
    case kMomoLogInfo: MOMO_LOG(INFO) << text; break;
    case kMomoLogWarn: MOMO_LOG(WARNING) << text; break;
    case kMomoLogError: MOMO_LOG(ERROR) << text; break;
  }
}

MomoStatus PlatformViewRegisterFactory(MomoEngineRef engine, const char* view_type,
                                       const MomoPlatformViewFactory* factory) {
  Core* core = Live(engine);
  if (core == nullptr || core->env.platform_views == nullptr) return kMomoErrNotFound;
  if (!core->env.runners.platform->RunsTasksOnCurrentThread()) return kMomoErrWrongThread;
  if (view_type == nullptr || factory == nullptr ||
      factory->struct_size < sizeof(MomoPlatformViewFactory) || factory->create == nullptr ||
      factory->dispose == nullptr) {
    return kMomoErrInvalidArgs;
  }
  const MomoPlatformViewFactory copy = *factory;
  PlatformViewRegistry::Factory entry;
  entry.owner = engine->id;
  entry.create = [copy](int64_t view_id, const std::vector<uint8_t>& params,
                        std::string*) -> void* {
    return copy.create(copy.user_data, view_id, params.empty() ? nullptr : params.data(),
                       params.size());
  };
  entry.dispose = [copy](int64_t view_id, void* native_view) {
    copy.dispose(copy.user_data, view_id, native_view);
  };
  Status status = core->env.platform_views->RegisterFactory(view_type, std::move(entry));
  if (!status.ok()) {
    MOMO_LOG(ERROR) << "Extension " << engine->id << ": " << status;
    return kMomoErrInvalidArgs;
  }
  return kMomoOk;
}
MomoStatus PlatformViewGet(MomoEngineRef engine, int64_t view_id, void** out) {
  Core* core = Live(engine);
  if (core == nullptr || core->env.platform_views == nullptr) return kMomoErrNotFound;
  if (out == nullptr) return kMomoErrInvalidArgs;
  if (!core->env.runners.platform->RunsTasksOnCurrentThread()) return kMomoErrWrongThread;
  *out = core->env.platform_views->Get(view_id);
  return *out != nullptr ? kMomoOk : kMomoErrNotFound;
}
MomoStatus BuilderAddFunctionUnavailable(MomoJsBuilderRef, const char*, MomoNativeFn, void*) {
  return kMomoErrInternal;  // 无 JS 运行时的构建
}

// ---- 动态库加载 ----

struct LoadedLibrary {
  void* handle = nullptr;
  MomoExtInitFn init = nullptr;
};

StatusOr<LoadedLibrary> LoadLibraryAt(const std::filesystem::path& path) {
  LoadedLibrary lib;
#if defined(_WIN32)
  HMODULE module = ::LoadLibraryW(path.wstring().c_str());
  if (module == nullptr) {
    return NotFoundError("LoadLibrary failed (" + std::to_string(::GetLastError()) + ")");
  }
  lib.handle = module;
  lib.init = reinterpret_cast<MomoExtInitFn>(::GetProcAddress(module, "momo_ext_init"));
#else
  // RTLD_LOCAL: 扩展的符号互不可见, 避免两个扩展内的同名静态库符号冲突。
  void* handle = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* error = ::dlerror();
    return NotFoundError(std::string("dlopen failed: ") + (error ? error : "?"));
  }
  lib.handle = handle;
  lib.init = reinterpret_cast<MomoExtInitFn>(::dlsym(handle, "momo_ext_init"));
#endif
  if (lib.init == nullptr) {
    return NotFoundError("library does not export momo_ext_init");
  }
  return lib;
}

}  // namespace

// ---------------------------------------------------------------------------

StatusOr<std::vector<ExtensionManifestEntry>> ParseExtensionManifest(std::string_view json) {
  MOMO_ASSIGN_OR_RETURN(JsonValue root, ParseJson(json));
  const JsonValue* list = root.Get("extensions");
  if (list == nullptr || list->AsArray() == nullptr) {
    return InvalidArgumentError("extensions.manifest: missing 'extensions' array");
  }
  std::vector<ExtensionManifestEntry> out;
  for (const JsonValue& item : *list->AsArray()) {
    ExtensionManifestEntry entry;
    entry.id = item.GetString("id");
    entry.library = item.GetString("library");
    entry.specifier = item.GetString("specifier");
    if (entry.id.empty() || entry.library.empty()) {
      return InvalidArgumentError("extensions.manifest: each entry needs 'id' and 'library'");
    }
    if (const JsonValue* caps = item.Get("capabilities"); caps && caps->AsArray()) {
      for (const JsonValue& cap : *caps->AsArray()) {
        if (const std::string* s = cap.AsString()) entry.capabilities.push_back(*s);
      }
    }
    out.push_back(std::move(entry));
  }
  return out;
}

ExtensionHost::ExtensionHost(Environment env) : core_(std::make_shared<Core>(std::move(env))) {
  MomoHostApi& api = core_->api;
  api.struct_size = sizeof(MomoHostApi);
  api.api_version = MOMO_EXT_API_VERSION;
  api.register_module = &RegisterModule;
  api.builder_add_function = &BuilderAddFunctionUnavailable;  // V8 胶水安装时替换
  api.call = &kStubCallApi;
  api.promise_resolve_bytes = &PromiseResolveBytes;
  api.promise_reject = &PromiseReject;
  api.channel_set_handler = &ChannelSetHandler;
  api.channel_reply = &ChannelReply;
  api.channel_send = &ChannelSend;
  api.post_task = &PostTask;
  api.post_delayed_task = &PostDelayedTask;
  api.get_window_handle = &GetWindowHandle;
  api.add_frame_callback = &AddFrameCallback;
  api.remove_frame_callback = &RemoveFrameCallback;
  api.texture_register = &TextureRegister;
  api.texture_mark_frame_available = &TextureMarkFrameAvailable;
  api.texture_unregister = &TextureUnregister;
  api.texture_update_pixels = &TextureUpdatePixels;
  api.open_asset = &OpenAsset;
  api.free_asset = &FreeAsset;
  api.log = &Log;
  api.platform_view_register_factory = &PlatformViewRegisterFactory;
  api.platform_view_get = &PlatformViewGet;
}

ExtensionHost::~ExtensionHost() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  DetachAll();
}

const MomoHostApi* ExtensionHost::host_api() const { return &core_->api; }

void ExtensionHost::SetJsGlue(const MomoCallApi* call_api, AddFunctionFn add_function) {
  core_->api.call = call_api != nullptr ? call_api : &kStubCallApi;
  core_->api.builder_add_function =
      add_function != nullptr ? add_function : &BuilderAddFunctionUnavailable;
}

Status ExtensionHost::LoadFromManifest() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  const std::filesystem::path& dir = core_->env.extensions_dir;
  std::error_code ec;
  const std::filesystem::path manifest_path = dir / kExtensionManifestName;
  if (dir.empty() || !std::filesystem::is_regular_file(manifest_path, ec)) {
    return Status::Ok();  // 无扩展
  }
  std::ifstream in(manifest_path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  MOMO_ASSIGN_OR_RETURN(std::vector<ExtensionManifestEntry> entries,
                        ParseExtensionManifest(ss.str()));
  const assets::AssetManager resolver(dir);
  for (const ExtensionManifestEntry& entry : entries) {
    StatusOr<std::filesystem::path> path = resolver.Resolve(entry.library);
    if (!path.ok()) {
      MutexLock lock(core_->mutex);
      core_->errors.push_back(entry.id + ": " + std::string(path.status().message()));
      continue;
    }
    StatusOr<LoadedLibrary> lib = LoadLibraryAt(*path);
    if (!lib.ok()) {
      MOMO_LOG(ERROR) << "Extension " << entry.id << " not loaded: " << lib.status();
      MutexLock lock(core_->mutex);
      core_->errors.push_back(entry.id + ": " + std::string(lib.status().message()));
      continue;
    }
    if (!entry.capabilities.empty()) {
      std::string caps;
      for (const std::string& c : entry.capabilities) caps += (caps.empty() ? "" : ", ") + c;
      MOMO_LOG(INFO) << "Extension " << entry.id << " requests capabilities: " << caps;
    }
    Attach(entry.id, lib->init, path->string()).IgnoreError();  // 失败已记入 errors
  }
  return Status::Ok();
}

Status ExtensionHost::AddExtension(const std::string& expected_id, MomoExtInitFn init) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  return Attach(expected_id, init, "<in-process>");
}

Status ExtensionHost::Attach(const std::string& expected_id, MomoExtInitFn init,
                             std::string origin) {
  auto fail = [this, &expected_id](std::string message) {
    MOMO_LOG(ERROR) << "Extension " << expected_id << " disabled: " << message;
    MutexLock lock(core_->mutex);
    core_->errors.push_back(expected_id + ": " + message);
    return FailedPreconditionError(std::move(message));
  };
  if (detached_) {
    return fail("host is shut down");
  }
  MomoExtension extension{};
  extension.struct_size = sizeof(MomoExtension);
  if (init == nullptr || !init(MOMO_EXT_API_VERSION, &extension)) {
    return fail("momo_ext_init rejected host API version " + std::to_string(MOMO_EXT_API_VERSION));
  }
  if (extension.id == nullptr || expected_id != extension.id) {
    return fail(std::string("id mismatch: manifest says '") + expected_id + "', library says '" +
                (extension.id ? extension.id : "<null>") + "'");
  }
  if (extension.on_attach == nullptr) {
    return fail("missing on_attach");
  }
  {
    MutexLock lock(core_->mutex);
    for (const auto& a : core_->attached) {
      if (a.id == expected_id) {
        return FailedPreconditionError("extension already attached: " + expected_id);
      }
    }
  }
  auto* engine = new MomoEngine{core_, expected_id};  // 有意不释放, 见头文件
  {
    MutexLock lock(core_->mutex);
    core_->attached.push_back({expected_id, extension, engine, false});
  }
  MOMO_LOG(INFO) << "Extension attached: " << expected_id << " (" << origin << ")";
  extension.on_attach(extension.user_data, &core_->api, engine);
  return Status::Ok();
}

void ExtensionHost::DetachAll() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (detached_) {
    return;
  }
  detached_ = true;
  std::vector<Core::Attached> attached;
  {
    MutexLock lock(core_->mutex);
    attached = core_->attached;
  }
  // 逆序 on_detach(此时扩展仍可调用宿主 API 注销资源)。
  for (auto it = attached.rbegin(); it != attached.rend(); ++it) {
    // 平台视图: 正常关闭流程中 Shell 已先行销毁全部视图(并从窗口移除);这里兜底, 保证 on_detach 之前
    // 该扩展创建的视图都已 dispose, 之后不再有人能用它的类型创建视图。
    if (core_->env.platform_views) {
      core_->env.platform_views->DisposeViewsOf(it->id);
      core_->env.platform_views->UnregisterFactoriesOf(it->id);
    }
    if (it->extension.on_detach != nullptr) {
      it->extension.on_detach(it->extension.user_data);
    }
    MOMO_LOG(INFO) << "Extension detached: " << it->id;
  }
  std::unique_ptr<WorkerPool> pool;
  std::map<uint64_t, MessageReply> replies;
  std::vector<std::string> channels;
  {
    MutexLock lock(core_->mutex);
    core_->alive = false;
    pool = std::move(core_->pool);
    replies = std::move(core_->replies);
    channels = std::move(core_->channels);
    core_->frame_callbacks.clear();
    core_->modules.clear();
    core_->promise_sink = nullptr;
  }
  for (const std::string& channel : channels) {
    core_->env.hub->SetNativeHandler(channel, nullptr, nullptr);
  }
  replies.clear();  // 未应答的消息回复 null
  if (pool) {
    pool->Shutdown();  // 排空已投递的扩展任务后 join
  }
}

std::vector<std::string> ExtensionHost::attached_ids() const {
  MutexLock lock(core_->mutex);
  std::vector<std::string> ids;
  for (const auto& a : core_->attached) ids.push_back(a.id);
  return ids;
}

std::vector<std::string> ExtensionHost::errors() const {
  MutexLock lock(core_->mutex);
  return core_->errors;
}

std::vector<ExtensionHost::ModuleRegistration> ExtensionHost::modules() const {
  MutexLock lock(core_->mutex);
  return core_->modules;
}

bool ExtensionHost::RunFrameCallbacks() {
  std::vector<Core::FrameCallback> callbacks;
  {
    MutexLock lock(core_->mutex);
    if (!core_->alive) return false;
    for (const auto& [id, cb] : core_->frame_callbacks) callbacks.push_back(cb);
  }
  for (const auto& cb : callbacks) {
    cb.fn(cb.user_data);
  }
  return has_frame_callbacks();
}

bool ExtensionHost::has_frame_callbacks() const {
  MutexLock lock(core_->mutex);
  return core_->alive && !core_->frame_callbacks.empty();
}

void ExtensionHost::SetPromiseSink(PromiseSink sink) {
  MutexLock lock(core_->mutex);
  core_->promise_sink = std::move(sink);
}

}  // namespace momo::engine
