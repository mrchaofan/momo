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
    platform::PlatformServices* services,
    std::vector<std::pair<std::string, MomoExtInitFn>> in_process_extensions) {
  if (!task_runners.IsValid()) {
    return InvalidArgumentError("Shell::Create requires all four task runners");
  }
  MOMO_DCHECK_RUNS_ON(task_runners.platform);
  std::unique_ptr<Shell> shell(new Shell(task_runners, settings, services));
  Shell* raw = shell.get();

  // raster 与 ui 并行初始化, platform 线程汇合等待。
  WaitableEvent raster_ready;
  task_runners.raster->PostTask([raw, &raster_ready] {
    raw->rasterizer_ = std::make_unique<Rasterizer>();
    raster_ready.Signal();
  });
  WaitableEvent ui_ready;
  Status ui_status;
  raw->RegisterPlatformChannels();

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

  ui::AppHost host;
  host.ui_runner = task_runners.ui;
  host.io_runner = task_runners.io;
  host.hub = raw->hub_;
  host.extensions = raw->extensions_.get();
  host.textures = raw->textures_;
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

Shell::Shell(const TaskRunners& task_runners, const Settings& settings,
             platform::PlatformServices* services)
    : Object(task_runners.platform),
      task_runners_(task_runners),
      refresh_rate_(settings.refresh_rate),
      hub_(std::make_shared<MessageChannelHub>()),
      textures_(std::make_shared<graphics::TextureRegistry>()),
      platform_views_(std::make_shared<PlatformViewRegistry>()),
      services_(services),
      weak_this_(this) {}

Shell::~Shell() { Shutdown(); }

void Shell::Shutdown() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_) {
    return;
  }
  shut_down_ = true;
  InvalidateWeakPtrs();
  for (const char* channel :
       {"momo/clipboard", "momo/url", "momo/textinput", "momo/platform_views"}) {
    hub_->SetNativeHandler(channel, nullptr, nullptr);
  }
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
  host_ = nullptr;
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

void Shell::SetHostDelegate(HostDelegate* host) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  host_ = host;
}

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
  if (focused_view_ == view_id) {
    focused_view_ = platform::kImplicitViewId;
  }
  if (view_id != platform::kImplicitViewId) {
    PostToEngine([view_id](Engine& engine) { engine.RemoveView(view_id); });
  }
  // 3) raster 停止使用 surface。surface 归窗口所有且被 raster 跨线程使用(唯一例外),
  //    窗口在本调用返回后就会销毁, 所以必须同步汇合。
  PostTaskAndWait(*task_runners_.raster, [this, view_id] {
    if (rasterizer_) {
      if (view_id == platform::kImplicitViewId) {
        rasterizer_->Teardown(view_id);  // 主窗口: 保留最近一帧, 重新绑定时补画
      } else {
        rasterizer_->RemoveView(view_id);
      }
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
  if (focused) {
    focused_view_ = view;
  }
  PostToEngine([view, focused](Engine& engine) { engine.OnFocusChanged(view, focused); });
}

void Shell::OnTextInput(const platform::TextInputEvent& event) {
  using Type = platform::TextInputEvent::Type;
  EncodableMap args;
  std::string method;
  switch (event.type) {
    case Type::kCommit:
      method = "commit";
      args.emplace_back("text", event.text);
      break;
    case Type::kComposing:
      method = "composing";
      args.emplace_back("text", event.text);
      args.emplace_back("cursor", event.cursor);
      break;
    case Type::kComposingEnd:
      method = "composingEnd";
      break;
    case Type::kCommand:
      method = "command";
      args.emplace_back("command", event.command);
      break;
  }
  if (event.replace_start >= 0 && event.replace_end >= event.replace_start) {
    args.emplace_back("replaceStart", event.replace_start);
    args.emplace_back("replaceEnd", event.replace_end);
  }
  hub_->SendToJs("momo/textinput", EncodeMethodCall({method, std::move(args)}));
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
  // 旧 JS 打开的其他窗口与创建的平台视图随运行时一起作废(新 bundle 从头创建, id 可能复用)。
  if (host_ != nullptr) {
    for (platform::ViewId view : attached_views()) {
      if (view != platform::kImplicitViewId) {
        host_->CloseView(view);
      }
    }
  }
  DisposeAllPlatformViews();
  PostToEngine([](Engine& engine) {
    if (Status status = engine.HotRestart(); !status.ok()) {
      MOMO_LOG(ERROR) << "Hot restart failed: " << status;
    }
  });
}

void Shell::NotifyLifecycleEvent(LifecycleEvent event) {
  PostToEngine([event](Engine& engine) { engine.OnLifecycleEvent(event); });
}

void Shell::RequestClose(MoveOnlyFunction<void(bool allow)> reply) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_) {
    reply(true);
    return;
  }
  // 捕获 this 安全: 同 PostToEngine。回复不依赖 Shell 存活: 若期间 Shell 已关闭,
  // 关闭流程已由别处推进, embedder 侧的回复处理需幂等。
  PostTaskAndReplyWithResult(
      *task_runners_.ui, [this] { return engine_ ? engine_->OnCloseRequested() : true; },
      task_runners_.platform, std::move(reply));
}

void Shell::RequestCloseView(platform::ViewId view, MoveOnlyFunction<void(bool allow)> reply) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (shut_down_) {
    reply(true);
    return;
  }
  PostTaskAndReplyWithResult(
      *task_runners_.ui, [this, view] { return engine_ ? engine_->OnCloseRequested(view) : true; },
      task_runners_.platform, std::move(reply));
}

// ---------------------------------------------------------------------------
// 引擎 → 宿主

void Shell::SetWindowTitle(platform::ViewId view, std::string title) {
  PostToDelegate(view, [title = std::move(title)](Delegate& d) { d.OnEngineSetTitle(title); });
}

void Shell::SetWindowMinSize(platform::ViewId view, Size logical_size) {
  PostToDelegate(view, [logical_size](Delegate& d) { d.OnEngineSetMinSize(logical_size); });
}

void Shell::SetCursor(platform::ViewId view, platform::SystemCursor cursor) {
  PostToDelegate(view, [cursor](Delegate& d) { d.OnEngineSetCursor(cursor); });
}

void Shell::SetWindowAlwaysOnTop(platform::ViewId view, bool on) {
  PostToDelegate(view, [on](Delegate& d) { d.OnEngineSetAlwaysOnTop(on); });
}

void Shell::SetWindowTitleBar(platform::ViewId view, bool hidden, double traffic_center_y) {
  PostToDelegate(view, [hidden, traffic_center_y](Delegate& d) {
    d.OnEngineSetTitleBar(hidden, traffic_center_y);
  });
}

void Shell::SetWindowDragRegions(platform::ViewId view, std::vector<Rect> rects) {
  PostToDelegate(view, [rects = std::move(rects)](Delegate& d) { d.OnEngineSetDragRegions(rects); });
}

void Shell::ActivateWindow(platform::ViewId view) {
  PostToDelegate(view, [](Delegate& d) { d.OnEngineActivate(); });
}

void Shell::CreateView(uint64_t request, const platform::ViewOptions& options) {
  // ui 线程 → platform 线程: 分配 id, 由宿主创建窗口并 AttachView(其 AddView 先于结果投递到 ui,
  // JS 在 Promise 结算时已能在 views 中看到新视图)。
  PostToObject(weak_this_, [request, options](Shell& self) {
    StatusOr<platform::ViewId> result = UnimplementedError("this host does not support createView");
    if (self.host_ != nullptr && !self.shut_down_) {
      const platform::ViewId id = self.next_view_id_++;
      Status status = self.host_->CreateView(id, options);
      result = status.ok() ? StatusOr<platform::ViewId>(id) : StatusOr<platform::ViewId>(status);
      if (status.ok() && self.DelegateOf(id) == nullptr) {
        result = InternalError("host created the window but did not attach view " + std::to_string(id));
      }
    }
    self.PostToEngine([request, result = std::move(result)](Engine& engine) mutable {
      engine.OnViewCreateResult(request, std::move(result));
    });
  });
}

void Shell::CloseView(platform::ViewId view) {
  PostToObject(weak_this_, [view](Shell& self) {
    if (self.host_ != nullptr && !self.shut_down_ && view != platform::kImplicitViewId) {
      self.host_->CloseView(view);
    }
  });
}

void Shell::RunWithPlatformServices(MoveOnlyFunction<void(platform::PlatformServices&)> task) {
  PostToObject(weak_this_, [task = std::move(task)](Shell& self) mutable {
    if (self.services_ != nullptr) {
      task(*self.services_);
    }
  });
}

// ---------------------------------------------------------------------------
// 平台服务通道

namespace {

// 在 platform 线程上执行方法调用并回复。handler 返回 StatusOr<结果>。
using MethodHandler = MoveOnlyFunction<StatusOr<EncodableValue>(const MethodCall&)>;

void ReplyTo(MessageReply& reply, StatusOr<EncodableValue> result) {
  if (result.ok()) {
    reply.Send(EncodeSuccessEnvelope(*result));
  } else {
    reply.Send(EncodeErrorEnvelope("error", result.status().message()));
  }
}

Rect RectArg(const EncodableValue& args) {
  Rect r;
  if (const auto* list = args.GetIf<EncodableList>(); list != nullptr && list->size() >= 4) {
    r = {(*list)[0].AsDouble().value_or(0), (*list)[1].AsDouble().value_or(0),
         (*list)[2].AsDouble().value_or(0), (*list)[3].AsDouble().value_or(0)};
  } else if (const auto* f = args.GetIf<std::vector<double>>(); f != nullptr && f->size() >= 4) {
    r = {(*f)[0], (*f)[1], (*f)[2], (*f)[3]};
  }
  return r;
}

platform::TextEditingState EditingStateArg(const EncodableValue& args) {
  platform::TextEditingState state;
  auto num = [&args](const char* key, int32_t fallback) {
    const EncodableValue* v = args.Find(key);
    return v != nullptr ? static_cast<int32_t>(v->AsInt().value_or(fallback)) : fallback;
  };
  if (const EncodableValue* text = args.Find("text")) {
    state.text = text->AsString().value_or("");
  }
  state.selection_base = num("selectionBase", 0);
  state.selection_extent = num("selectionExtent", state.selection_base);
  state.composing_start = num("composingStart", -1);
  state.composing_end = num("composingEnd", -1);
  return state;
}

}  // namespace

void Shell::RegisterPlatformChannels() {
  // handler 在 platform 线程运行, 经 weak_this_ 判活(Shell 关闭后 WeakPtr 失效, 回复错误)。
  auto add = [this](const char* channel,
                    std::function<StatusOr<EncodableValue>(Shell&, const MethodCall&)> fn) {
    WeakPtr<Shell> weak = weak_this_;
    hub_->SetNativeHandler(
        channel, task_runners_.platform,
        [weak, fn = std::move(fn)](MessageBytes message, MessageReply reply) {
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
          ReplyTo(reply, fn(*self, *call));
        });
  };

  add("momo/clipboard", [](Shell& self, const MethodCall& call) -> StatusOr<EncodableValue> {
    if (self.services_ == nullptr) {
      return UnavailableError("no platform services");
    }
    if (call.method == "getText") {
      MOMO_ASSIGN_OR_RETURN(std::string text, self.services_->GetClipboardText());
      return EncodableValue(std::move(text));
    }
    if (call.method == "setText") {
      MOMO_RETURN_IF_ERROR(self.services_->SetClipboardText(call.args.AsString().value_or("")));
      return EncodableValue();
    }
    return UnimplementedError("momo/clipboard: unknown method " + call.method);
  });

  add("momo/url", [](Shell& self, const MethodCall& call) -> StatusOr<EncodableValue> {
    if (self.services_ == nullptr) {
      return UnavailableError("no platform services");
    }
    if (call.method == "open") {
      MOMO_RETURN_IF_ERROR(self.services_->OpenUrl(call.args.AsString().value_or("")));
      return EncodableValue();
    }
    return UnimplementedError("momo/url: unknown method " + call.method);
  });

  RegisterPlatformViewChannel();

  add("momo/textinput", [](Shell& self, const MethodCall& call) -> StatusOr<EncodableValue> {
    // 输入法连接属于当前获得焦点的窗口(没有则主窗口)。
    Delegate* delegate = self.DelegateOf(self.focused_view_);
    if (delegate == nullptr) delegate = self.DelegateOf(platform::kImplicitViewId);
    platform::TextInputConnection* input = delegate != nullptr ? delegate->GetTextInput() : nullptr;
    if (input == nullptr) {
      return EncodableValue();  // 无窗口: 静默忽略(headless)
    }
    if (call.method == "show") {
      input->Show();
    } else if (call.method == "hide") {
      input->Hide();
    } else if (call.method == "setCaretRect") {
      input->SetCaretRect(RectArg(call.args));
    } else if (call.method == "setEditingState") {
      input->SetEditingState(EditingStateArg(call.args));
    } else {
      return UnimplementedError("momo/textinput: unknown method " + call.method);
    }
    return EncodableValue();
  });
}

// ---------------------------------------------------------------------------
// 平台视图通道(momo/platform_views, JS→平台):
//   create  {id: int, viewType: string, params: bytes|null}
//   dispose {id: int}
//   focus   {id: int}

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
