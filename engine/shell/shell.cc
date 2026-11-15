#include "engine/shell/shell.h"

#include <utility>

#include "base/logging.h"
#include "base/task_util.h"
#include "engine/common/standard_codec.h"
#include "engine/shell/rasterizer.h"

namespace momo::engine {

// ---------------------------------------------------------------------------
// 创建与关闭

StatusOr<std::unique_ptr<Shell>> Shell::Create(const TaskRunners& task_runners,
                                               const Settings& settings,
                                               std::function<void(int)> on_exit_requested) {
  if (!task_runners.IsValid()) {
    return InvalidArgumentError("Shell::Create requires all four task runners");
  }
  MOMO_DCHECK_RUNS_ON(task_runners.platform);
  std::unique_ptr<Shell> shell(new Shell(task_runners, settings));
  Shell* raw = shell.get();
  WeakPtr<Shell> weak = raw->weak_this_;

  // raster 与 ui 两侧各自在自己的线程上构造;不等待——之后对它们的访问都是排在后面的任务。
  // 捕获 raw 安全: Shutdown 在两条线程上都同步汇合, Shell 之后才析构。
  task_runners.raster->PostTask([raw, weak] {
    raw->rasterizer_ = std::make_unique<Rasterizer>();
    // raster 线程回调, 投递回 platform 交给视图的宿主。
    raw->rasterizer_->SetFirstFrameCallback([weak](platform::ViewId view) {
      PostToObject(weak, [view](Shell& self) {
        if (Delegate* delegate = self.DelegateOf(view)) delegate->OnFirstFramePresented();
      });
    });
  });
  raw->RegisterEngineChannels();

  // 扩展: manifest 声明的动态库, on_attach 在本(platform)线程(docs/07 §5)。
  ExtensionHost::Environment env;
  env.runners = task_runners;
  env.hub = raw->hub_;
  env.textures = raw->textures_;
  env.extensions_dir = settings.extensions_dir;
  env.assets_dir = settings.assets_dir;
  env.platform_views = raw->platform_views_;
  env.window_handle = [raw](int64_t view_id) -> void* {
    Delegate* delegate = raw->DelegateOf(view_id);
    return delegate != nullptr ? delegate->GetNativeWindowHandle() : nullptr;
  };
  env.js_glue = ui::GetExtensionJsGlue();
  raw->extensions_ = std::make_unique<ExtensionHost>(std::move(env));
  if (Status status = raw->extensions_->LoadFromManifest(); !status.ok()) {
    MOMO_LOG(ERROR) << "Extensions manifest: " << status;
  }

  ui::AppHost host;
  host.io_runner = task_runners.io;
  host.hub = raw->hub_;
  host.extensions = raw->extensions_.get();
  host.textures = raw->textures_;
  if (on_exit_requested) {
    // ui 线程(JS 调用栈内)调用: 投递到 platform, 由宿主做有序关闭(它会与 ui 线程同步汇合, 不能在这里等)。
    host.request_exit = [weak, cb = std::move(on_exit_requested)](int code) {
      PostToObject(weak, [cb, code](Shell&) { cb(code); });
    };
  }
  // Engine 归属 ui 线程: 在那里构造(没有 JS 运行时;Run 时才创建)。
  task_runners.ui->PostTask([raw, settings, host = std::move(host)] {
    raw->engine_ = std::make_unique<Engine>(
        settings,
        [raw](std::shared_ptr<LayerTreePipeline> pipeline) { raw->OnEngineDraw(std::move(pipeline)); },
        raw->MakeVsyncWaiter({}), host);
  });
  return shell;
}

Shell::Shell(const TaskRunners& task_runners, const Settings& settings)
    : Object(task_runners.platform),
      task_runners_(task_runners),
      refresh_rate_(settings.refresh_rate),
      hub_(std::make_shared<MessageChannelHub>()),
      textures_(std::make_shared<graphics::TextureRegistry>()),
      platform_views_(std::make_shared<PlatformViewRegistry>()),
      weak_this_(this) {}

Shell::~Shell() { Shutdown(); }

void Shell::Shutdown() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_) {
    return;
  }
  shut_down_ = true;
  InvalidateWeakPtrs();
  hub_->SetNativeHandler("momo/platform_views", nullptr, nullptr);
  hub_->SetNativeHandler("momo/extensions", nullptr, nullptr);
  // 1) JS 运行时: 与 hot restart 相同的拆除顺序(运行时销毁 → 平台视图 → RuntimeStopped)。
  StopRuntime(kMomoLifecycleReasonShutdown);
  // 2) ui: 销毁 Engine(Animator)。此前投递到 ui 的任务按 FIFO 已执行完毕, 之后不再投递。
  //    必须在线程 join 前同步销毁(docs/01 §4), 所以这里同步汇合。
  PostTaskAndWait(*task_runners_.ui, [this] { engine_.reset(); });
  // 3) 扩展 on_detach(platform 线程, 线程 join 之前;docs/08 §5)。
  if (extensions_) {
    extensions_->DetachAll();
  }
  // 释放 VsyncSource(DeleteLater, platform 事件循环安全点析构)。
  delegates_.clear();
  vsync_source_.reset();
  // 4) raster: 释放 LayerTree 与 GPU 资源(含解绑 surface)。ui 已停, 不会再有新的 Draw 投递。
  PostTaskAndWait(*task_runners_.raster, [this] { rasterizer_.reset(); });
  MOMO_LOG(INFO) << "Shell shut down.";
}

// ---------------------------------------------------------------------------
// JS 运行时的启动与停止

void Shell::Run(RunCallback done) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(runtime_generation_ == 0) << "Shell::Run may only be called once";
  if (shut_down_ || runtime_started_) {
    return;
  }
  StartRuntime(kMomoLifecycleReasonLaunch, std::move(done));
}

void Shell::RequestHotRestart(RunCallback done) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_ || !runtime_started_) {
    return;
  }
  MOMO_LOG(INFO) << "Hot restart.";
  StopRuntime(kMomoLifecycleReasonHotRestart);
  if (!done) {
    done = [](Status status) {
      if (!status.ok()) MOMO_LOG(ERROR) << "Hot restart failed: " << status;
    };
  }
  StartRuntime(kMomoLifecycleReasonHotRestart, std::move(done));
}

void Shell::StartRuntime(MomoLifecycleReason reason, RunCallback done) {
  MOMO_DCHECK(!runtime_started_);
  runtime_started_ = true;
  const uint64_t generation = ++runtime_generation_;
  // 此刻没有 JS 运行时: 扩展先得知新一代即将开始, 它发来的消息一定晚于这个事件。
  extensions_->NotifyLifecycle(kMomoLifecycleRuntimeStarting, reason, generation);
  // 捕获 this 安全: 同 PostToEngine。结果经弱引用回到 platform(Shell 已关闭则丢弃)。
  task_runners_.ui->PostTask(
      [this, generation, weak = weak_this_, done = std::move(done)]() mutable {
        if (engine_ == nullptr) {
          return;
        }
        Status status = engine_->StartRuntime(generation);
        if (done) {
          PostToObject(weak, [done = std::move(done), status = std::move(status)](Shell&) mutable {
            done(std::move(status));
          });
        }
      });
}

void Shell::StopRuntime(MomoLifecycleReason reason) {
  if (!runtime_started_) {
    return;
  }
  runtime_started_ = false;
  // 1) ui: 销毁运行时。同步汇合: 返回后没有 JS 在运行(排在前面、尚未执行的 StartRuntime 也已执行完),
  //    引擎不再出帧、不再执行扩展帧回调, 旧运行时在途的通道消息已作废。
  PostTaskAndWait(*task_runners_.ui, [this] {
    if (engine_) engine_->StopRuntime();
  });
  // 2) 旧运行时创建的平台视图: 先移出窗口再交给扩展释放(JS 已不在, 不会与扩展的 JS 函数并发)。
  DisposeAllPlatformViews();
  // 3) 扩展清理属于这一代运行时的状态。
  extensions_->NotifyLifecycle(kMomoLifecycleRuntimeStopped, reason, runtime_generation_);
}

std::unique_ptr<VsyncWaiter> Shell::MakeVsyncWaiter(WeakPtr<platform::VsyncSource> source) const {
  if (source.is_bound()) {
    return std::make_unique<PlatformVsyncWaiter>(task_runners_.ui, std::move(source));
  }
  return std::make_unique<TimerVsyncWaiter>(task_runners_.ui, refresh_rate_);
}

// ---------------------------------------------------------------------------
// 视图绑定

Shell::Delegate* Shell::DelegateOf(platform::ViewId view) const {
  auto it = delegates_.find(view);
  return it != delegates_.end() ? it->second : nullptr;
}

std::vector<platform::ViewId> Shell::attached_views() const {
  std::vector<platform::ViewId> out;
  for (const auto& [view, delegate] : delegates_) {
    out.push_back(view);
  }
  return out;
}

void Shell::SetVsyncSource(ObjectPtr<platform::VsyncSource> source) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_) {
    return;  // source 随参数释放(DeleteLater)
  }
  // 旧来源 DeleteLater(即使身处其回调栈中也安全);ui 只持弱引用, 每次请求都回到 platform 线程判活,
  // 在途请求判活失败后作废, 无需与 platform 同步销毁。
  vsync_source_ = std::move(source);
  WeakPtr<platform::VsyncSource> weak = vsync_source_ ? WeakPtr<platform::VsyncSource>(vsync_source_.get())
                                                      : WeakPtr<platform::VsyncSource>();
  PostToEngine([this, weak](Engine& engine) { engine.SetVsyncWaiter(MakeVsyncWaiter(weak)); });
}

void Shell::AttachView(platform::ViewId view_id, Delegate* delegate, platform::RenderSurface* surface) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_ || delegate == nullptr) {
    return;
  }
  MOMO_DCHECK(delegates_.count(view_id) == 0) << "View " << view_id << " is already attached";
  delegates_[view_id] = delegate;
  // 已创建的平台视图: 补交给新窗口(由合成帧决定它最终出现在哪个窗口)。
  for (const PlatformViewRegistry::View& view : platform_views_->views()) {
    delegate->OnPlatformViewCreated(view.id, view.native);
  }
  PostToEngine([view_id](Engine& engine) { engine.AddView(view_id); });
  task_runners_.raster->PostTask([this, view_id, surface] {
    if (rasterizer_) {
      rasterizer_->Setup(view_id, surface);
    }
  });
}

void Shell::DetachView(platform::ViewId view_id) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_) {
    return;
  }
  Delegate* delegate = DelegateOf(view_id);
  if (delegate == nullptr) {
    return;
  }
  // 1) 平台视图移出即将解绑的窗口(视图本身保留, 仍可出现在其他窗口)。
  for (const PlatformViewRegistry::View& view : platform_views_->views()) {
    delegate->OnPlatformViewDisposed(view.id);
  }
  // 2) 解绑 delegate(帧时钟是应用级的, 不随视图解绑而变)。
  delegates_.erase(view_id);
  PostToEngine([view_id](Engine& engine) { engine.RemoveView(view_id); });
  // 3) raster 停止使用 surface 并丢弃该视图的帧。surface 归窗口所有且被 raster 跨线程使用(唯一例外),
  //    窗口在本调用返回后就会销毁, 所以必须同步汇合。
  PostTaskAndWait(*task_runners_.raster, [this, view_id] {
    if (rasterizer_) {
      rasterizer_->RemoveView(view_id);
    }
  });
}

// ---------------------------------------------------------------------------
// 跨线程投递

void Shell::PostToEngine(MoveOnlyFunction<void(Engine&)> task) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_) {
    return;
  }
  // 捕获 this 安全: Shutdown 在 ui 线程上排在这些任务之后同步执行, Shell 之后才析构。
  task_runners_.ui->PostTask([this, task = std::move(task)]() mutable {
    if (engine_) {
      task(*engine_);
    }
  });
}

void Shell::PostToDelegate(platform::ViewId view, MoveOnlyFunction<void(Delegate&)> task) {
  PostToObject(weak_this_, [view, task = std::move(task)](Shell& self) mutable {
    if (Delegate* delegate = self.DelegateOf(view)) {
      task(*delegate);
    }
  });
}

// ---------------------------------------------------------------------------
// 宿主事件入口

void Shell::OnMetricsChanged(platform::ViewId view, const platform::ViewportMetrics& metrics) {
  PostToEngine([view, metrics](Engine& engine) { engine.OnMetricsChanged(view, metrics); });
}

void Shell::OnPointerPacket(platform::ViewId view, platform::PointerDataPacket packet) {
  PostToEngine([view, packet = std::move(packet)](Engine& engine) {
    engine.OnPointerPacket(view, packet);
  });
}

void Shell::OnKeyData(platform::ViewId view, const platform::KeyData& data) {
  PostToEngine([view, data](Engine& engine) { engine.OnKeyData(view, data); });
}

void Shell::OnFocusChanged(platform::ViewId view, bool focused) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  PostToEngine([view, focused](Engine& engine) { engine.OnFocusChanged(view, focused); });
}

void Shell::OnViewVisibilityChanged(platform::ViewId view, bool visible) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  PostToEngine([view, visible](Engine& engine) { engine.OnVisibilityChanged(view, visible); });
}

void Shell::OnPlatformViewFocused(int64_t view_id) {
  hub_->SendToJs("momo/platform_views",
                 EncodeMethodCall({"focused", EncodableValue(EncodableMap{{"id", view_id}})}));
}

void Shell::DisposeAllPlatformViews() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  platform_views_->DisposeAll([this](int64_t view_id) {
    for (const auto& [view, delegate] : delegates_) {
      delegate->OnPlatformViewDisposed(view_id);
    }
  });
}

void Shell::SetAppSuspended(bool suspended) {
  PostToEngine([suspended](Engine& engine) { engine.SetAppSuspended(suspended); });
}

// ---------------------------------------------------------------------------
// 平台视图通道(momo/platform_views, JS→平台):
//   create  {id: int, viewType: string, params: bytes|null}
//   dispose {id: int}
//   focus   {id: int}
// 平台视图表归引擎(扩展经 C ABI 注册工厂, 合成帧按 LayerTree 中的位置切片), 所以这个通道由 Shell 处理;
// 原生视图挂进哪个窗口由宿主决定(Shell::Delegate 的平台视图回调)。
//
// 扩展通道(momo/extensions, JS→引擎):
//   getErrors  → string[]   加载失败的扩展("<id>: <原因>");全部成功为空数组
// 这是可查询的状态而不是事件: 任何时刻、任何一代 JS 运行时都能问到同样的结果。

namespace {

void ReplyTo(MessageReply& reply, StatusOr<EncodableValue> result) {
  if (result.ok()) {
    reply.Send(EncodeSuccessEnvelope(*result));
  } else {
    reply.Send(EncodeErrorEnvelope("error", result.status().message()));
  }
}

}  // namespace

void Shell::RegisterEngineChannels() {
  using Handler = StatusOr<EncodableValue> (Shell::*)(const MethodCall&);
  const auto bind = [this](const char* channel, Handler handler) {
    hub_->SetNativeHandler(channel, task_runners_.platform,
                           [weak = weak_this_, handler](MessageBytes message, MessageReply reply) {
                             StatusOr<MethodCall> call = DecodeMethodCall(message);
                             if (!call.ok()) {
                               ReplyTo(reply, call.status());
                               return;
                             }
                             Shell* self = weak.get();
                             if (self == nullptr || self->shut_down_) {
                               ReplyTo(reply, UnavailableError("engine is shutting down"));
                               return;
                             }
                             ReplyTo(reply, (self->*handler)(*call));
                           });
  };
  bind("momo/platform_views", &Shell::HandlePlatformViewCall);
  bind("momo/extensions", &Shell::HandleExtensionsCall);
}

StatusOr<EncodableValue> Shell::HandleExtensionsCall(const MethodCall& call) {
  if (call.method == "getErrors") {
    EncodableList errors;
    for (std::string& error : extensions_->errors()) {
      errors.emplace_back(std::move(error));
    }
    return EncodableValue(std::move(errors));
  }
  return UnimplementedError("momo/extensions: unknown method " + call.method);
}

StatusOr<EncodableValue> Shell::HandlePlatformViewCall(const MethodCall& call) {
  const EncodableValue* id_value = call.args.Find("id");
  const int64_t id = id_value != nullptr ? id_value->AsInt().value_or(0) : 0;
  if (id <= 0) {
    return InvalidArgumentError("momo/platform_views: missing positive 'id'");
  }
  if (call.method == "create") {
    const EncodableValue* type = call.args.Find("viewType");
    const std::string view_type = type != nullptr ? type->AsString().value_or("") : "";
    std::vector<uint8_t> params;
    if (const EncodableValue* p = call.args.Find("params")) {
      if (const auto* bytes = p->GetIf<std::vector<uint8_t>>()) {
        params = *bytes;
      }
    }
    MOMO_ASSIGN_OR_RETURN(void* native, platform_views_->Create(id, view_type, params));
    for (const auto& [view, delegate] : delegates_) {
      delegate->OnPlatformViewCreated(id, native);
    }
    return EncodableValue();
  }
  if (call.method == "dispose") {
    MOMO_RETURN_IF_ERROR(platform_views_->Dispose(id, [this](int64_t view_id) {
      for (const auto& [view, delegate] : delegates_) {
        delegate->OnPlatformViewDisposed(view_id);
      }
    }));
    return EncodableValue();
  }
  if (call.method == "focus") {
    if (platform_views_->Get(id) == nullptr) {
      return NotFoundError("no platform view " + std::to_string(id));
    }
    // 只有当前承载该原生视图的窗口会响应。
    for (const auto& [view, delegate] : delegates_) {
      delegate->OnPlatformViewFocusRequested(id);
    }
    return EncodableValue();
  }
  return UnimplementedError("momo/platform_views: unknown method " + call.method);
}

// ---------------------------------------------------------------------------
// 帧路径

void Shell::OnEngineDraw(std::shared_ptr<LayerTreePipeline> pipeline) {
  // 捕获 this 安全: 本任务由 ui 线程投递, Shutdown 先同步排空 ui, 再在 raster 上排到这些任务之后。
  task_runners_.raster->PostTask([this, pipeline = std::move(pipeline)] {
    if (rasterizer_) {
      rasterizer_->Draw(*pipeline);
    }
  });
}

}  // namespace momo::engine
