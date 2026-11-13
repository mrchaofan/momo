#include "engine/shell/shell.h"

#include <utility>

#include "base/logging.h"
#include "base/task_util.h"
#include "base/waitable_event.h"
#include "engine/common/standard_codec.h"
#include "engine/shell/rasterizer.h"

namespace momo::engine {

// ---------------------------------------------------------------------------
// 创建与关闭

StatusOr<std::unique_ptr<Shell>> Shell::Create(
    const TaskRunners& task_runners, const Settings& settings,
    const std::function<void(Shell&)>& on_created, std::function<void(int)> on_exit_requested,
    std::vector<std::pair<std::string, MomoExtInitFn>> in_process_extensions) {
  if (!task_runners.IsValid()) {
    return InvalidArgumentError("Shell::Create requires all four task runners");
  }
  MOMO_DCHECK_RUNS_ON(task_runners.platform);
  std::unique_ptr<Shell> shell(new Shell(task_runners, settings));
  Shell* raw = shell.get();

  // raster 与 ui 并行初始化, platform 线程汇合等待。
  WaitableEvent raster_ready;
  WeakPtr<Shell> weak_shell = raw->weak_this_;
  task_runners.raster->PostTask([raw, weak_shell, &raster_ready] {
    raw->rasterizer_ = std::make_unique<Rasterizer>();
    // raster 线程回调, 投递回 platform 交给视图的宿主。
    raw->rasterizer_->SetFirstFrameCallback([weak_shell](platform::ViewId view) {
      PostToObject(weak_shell, [view](Shell& self) {
        if (Delegate* delegate = self.DelegateOf(view)) delegate->OnFirstFramePresented();
      });
    });
    raster_ready.Signal();
  });
  WaitableEvent ui_ready;
  Status ui_status;
  raw->RegisterPlatformViewChannel();

  // 扩展: manifest 声明的动态库 + 进程内扩展, on_attach 在本(platform)线程(docs/07 §5)。
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
  WeakPtr<Shell> weak = raw->weak_this_;
  env.request_frame = [weak] {
    PostToObject(weak, [](Shell& self) {
      self.PostToEngine([](Engine& engine) { engine.ScheduleFrameFromHost(); });
    });
  };
  // 两个回调都在 platform 线程(注册方)上执行: 纹理新帧经 TextureRegistry 投递过来,
  // request_frame 由 ExtensionHost 投递过来。
  raw->textures_->SetFrameListener(task_runners.platform,
                                   [request = env.request_frame](int64_t) { request(); });
  raw->extensions_ = std::make_unique<ExtensionHost>(std::move(env));
  if (Status status = raw->extensions_->LoadFromManifest(); !status.ok()) {
    MOMO_LOG(ERROR) << "Extensions manifest: " << status;
  }
  for (auto& [id, init] : in_process_extensions) {
    raw->extensions_->AddExtension(id, init).IgnoreError();  // 失败记入 errors()
  }

  // 宿主的通道处理器先于 JS bundle 就位。
  if (on_created) {
    on_created(*raw);
  }

  ui::AppHost host;
  host.ui_runner = task_runners.ui;
  host.io_runner = task_runners.io;
  host.hub = raw->hub_;
  host.extensions = raw->extensions_.get();
  host.textures = raw->textures_;
  if (on_exit_requested) {
    // ui 线程(JS 调用栈内)调用: 投递到 platform, 由宿主做有序关闭(它会与 ui 线程同步汇合, 不能在这里等)。
    host.request_exit = [weak = raw->weak_this_, cb = std::move(on_exit_requested)](int code) {
      PostToObject(weak, [cb, code](Shell&) { cb(code); });
    };
  }
  task_runners.ui->PostTask([raw, &settings, &host, &ui_status, &ui_ready] {
    Engine::Delegate& delegate = *raw;  // 私有基类: 在成员上下文内转换
    auto engine = Engine::Create(settings, delegate, raw->MakeVsyncWaiter({}), host);
    if (engine.ok()) {
      raw->engine_ = std::move(engine).value();
    } else {
      ui_status = engine.status();
    }
    ui_ready.Signal();
  });
  raster_ready.Wait();
  ui_ready.Wait();

  if (!ui_status.ok()) {
    shell->Shutdown();
    return ui_status;
  }
  // 扩展加载错误经 momo/extensions 通道报给 JS(bundle 已执行, handler 已可注册;docs/07 §5)。
  for (const std::string& error : shell->extensions_->errors()) {
    shell->hub_->SendToJs("momo/extensions", EncodeStandard(EncodableValue(error)));
  }
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
  // 1) ui: 销毁 Engine(JS 运行时 → Animator)。此前投递到 ui 的任务按 FIFO 已执行完毕, 之后不再投递。
  //    JS 运行时必须在线程 join 前同步销毁(docs/01 §4), 所以这里同步汇合。
  PostTaskAndWait(*task_runners_.ui, [this] { engine_.reset(); });
  // 平台视图: JS 已不在, 先移出窗口再交给扩展释放(必须早于扩展 on_detach)。
  DisposeAllPlatformViews();
  // 扩展 on_detach(platform 线程, JS 运行时已销毁, 线程 join 之前;docs/08 §5)。
  if (extensions_) {
    extensions_->DetachAll();
  }
  textures_->SetFrameListener(nullptr, nullptr);
  // 释放 VsyncSource(DeleteLater, platform 事件循环安全点析构)。
  delegates_.clear();
  active_vsync_ = nullptr;
  vsync_sources_.clear();
  // 2) raster: 释放 LayerTree 与 GPU 资源(含解绑 surface)。ui 已停, 不会再有新的 Draw 投递。
  PostTaskAndWait(*task_runners_.raster, [this] { rasterizer_.reset(); });
  MOMO_LOG(INFO) << "Shell shut down.";
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

void Shell::UpdateVsyncSource() {
  platform::VsyncSource* chosen = nullptr;
  auto main = vsync_sources_.find(platform::kImplicitViewId);
  if (main != vsync_sources_.end() && main->second) {
    chosen = main->second.get();
  } else {
    for (const auto& [view, source] : vsync_sources_) {
      if (source) {
        chosen = source.get();
        break;
      }
    }
  }
  if (chosen == active_vsync_) {
    return;
  }
  active_vsync_ = chosen;
  // ui 只持弱引用, 每次请求都回到 platform 线程判活, 无需与 platform 同步销毁。
  WeakPtr<platform::VsyncSource> weak =
      chosen != nullptr ? WeakPtr<platform::VsyncSource>(chosen) : WeakPtr<platform::VsyncSource>();
  PostToEngine([this, weak](Engine& engine) { engine.SetVsyncWaiter(MakeVsyncWaiter(weak)); });
}

void Shell::AttachView(platform::ViewId view_id, Delegate* delegate,
                       platform::RenderSurface* surface, ObjectPtr<platform::VsyncSource> vsync) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_ || delegate == nullptr) {
    return;
  }
  MOMO_DCHECK(delegates_.count(view_id) == 0) << "View " << view_id << " is already attached";
  delegates_[view_id] = delegate;
  if (vsync) {
    vsync_sources_[view_id] = std::move(vsync);
  }
  UpdateVsyncSource();
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
  // 2) 解绑 delegate, 释放 VsyncSource —— DeleteLater, 即使身处其回调栈中也安全;
  //    帧时钟改用其他视图的来源(或定时器), ui 侧在途请求判活失败后作废。
  delegates_.erase(view_id);
  auto source = vsync_sources_.find(view_id);
  if (source != vsync_sources_.end()) {
    if (source->second.get() == active_vsync_) {
      active_vsync_ = nullptr;
    }
    vsync_sources_.erase(source);
  }
  UpdateVsyncSource();
  if (active_vsync_ == nullptr && vsync_sources_.empty()) {
    // UpdateVsyncSource 在"无变化"时不投递: 显式切回定时器。
    PostToEngine([this](Engine& engine) { engine.SetVsyncWaiter(MakeVsyncWaiter({})); });
  }
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

void Shell::RequestHotRestart() {
  // 旧 JS 创建的平台视图随运行时一起作废(新 bundle 从头创建, id 可能复用)。
  DisposeAllPlatformViews();
  PostToEngine([](Engine& engine) {
    if (Status status = engine.HotRestart(); !status.ok()) {
      MOMO_LOG(ERROR) << "Hot restart failed: " << status;
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

namespace {

void ReplyTo(MessageReply& reply, StatusOr<EncodableValue> result) {
  if (result.ok()) {
    reply.Send(EncodeSuccessEnvelope(*result));
  } else {
    reply.Send(EncodeErrorEnvelope("error", result.status().message()));
  }
}

}  // namespace

void Shell::RegisterPlatformViewChannel() {
  WeakPtr<Shell> weak = weak_this_;
  hub_->SetNativeHandler(
      "momo/platform_views", task_runners_.platform,
      [weak](MessageBytes message, MessageReply reply) {
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
        ReplyTo(reply, self->HandlePlatformViewCall(*call));
      });
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
