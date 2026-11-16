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
  raw->RegisterPlatformViewChannel();

  ui::AppHost host;
  host.io_runner = task_runners.io;
  host.hub = raw->hub_;
  host.native_host = raw->native_host_.table();
  host.textures = raw->textures_;
  if (on_exit_requested) {
    // ui 线程(JS 调用栈内)调用: 投递到 platform, 由宿主做有序关闭(它会与 ui 线程同步汇合, 不能在这里等)。
    host.request_exit = [weak, cb = std::move(on_exit_requested)](int code) {
      PostToObject(weak, [cb, code](Shell&) { cb(code); });
    };
  }
  // Engine 归属 ui 线程: 在那里构造(没有 JS 运行时;Run 时才创建)。
  // 帧管道: ui 线程生产(Animator), raster 线程消费(Rasterizer), 整个引擎就这一个。
  auto pipeline = LayerTreePipeline::Create(Animator::kPipelineDepth);
  task_runners.ui->PostTask([raw, settings, pipeline, host = std::move(host)] {
    // on_draw 在 ui 线程调用: 让 raster 把管道里的帧画出来。
    Closure on_draw = [raw, pipeline] {
      raw->PostToRasterizer([pipeline](Rasterizer& rasterizer) { rasterizer.Draw(*pipeline); });
    };
    raw->engine_ = std::make_unique<Engine>(settings, pipeline, std::move(on_draw),
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
      native_host_(task_runners.platform, textures_, platform_views_),
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
  // 原生扩展(Node 插件)的代码随 JS 运行时一起结束, 所以引擎对它们的调用要先结束(docs/07 §4):
  // 1) 关闭宿主能力表: 扩展此后的调用都被拒绝, 已投递而未执行的平台任务不再执行。
  native_host_.Close();
  // 2) 销毁全部平台视图(先移出窗口, 再交给扩展的工厂释放)并清空工厂。此后引擎不再调用任何扩展代码。
  platform_views_->Clear([this](int64_t view_id) {
    for (const auto& [view, delegate] : delegates_) {
      delegate->OnPlatformViewDisposed(view_id);
    }
  });
  // 3) ui: 销毁 Engine(JS 运行时与 Animator);扩展登记的清理函数在这里执行。此前投递到 ui 的任务按 FIFO
  //    已执行完毕(包括尚未执行的 Run), 之后不再投递。必须在线程 join 前同步销毁(docs/01 §4), 所以这里
  //    同步汇合。返回后没有 JS 在运行, 引擎不再出帧。
  PostTaskAndWait(*task_runners_.ui, [this] { engine_.reset(); });
  // 释放 VsyncSource(DeleteLater, platform 事件循环安全点析构)。
  delegates_.clear();
  vsync_source_.reset();
  // 4) raster: 释放 LayerTree 与 GPU 资源(含解绑 surface)。ui 已停, 不会再有新的 Draw 投递。
  PostTaskAndWait(*task_runners_.raster, [this] { rasterizer_.reset(); });
  MOMO_LOG(INFO) << "Shell shut down.";
}

// ---------------------------------------------------------------------------
// JS 运行时

void Shell::Run(RunCallback done) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  MOMO_DCHECK(!run_called_) << "Shell::Run may only be called once";
  if (shut_down_ || run_called_) {
    return;
  }
  run_called_ = true;
  // 结果经弱引用回到 platform(Shell 已关闭则丢弃)。
  PostToEngine([weak = weak_this_, done = std::move(done)](Engine& engine) mutable {
    Status status = engine.StartRuntime();
    if (done) {
      PostToObject(weak, [done = std::move(done), status = std::move(status)](Shell&) mutable {
        done(std::move(status));
      });
    }
  });
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
  PostToRasterizer([view_id, surface](Rasterizer& rasterizer) { rasterizer.Setup(view_id, surface); });
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

void Shell::PostToRasterizer(MoveOnlyFunction<void(Rasterizer&)> task) {
  // 任意线程(platform 或 ui)。捕获 this 安全: Shutdown 先同步排空 ui, 再在 raster 上排到这些任务之后同步销毁
  // Rasterizer, Shell 之后才析构;所以这里的任务要么先于销毁执行, 要么发现 rasterizer_ 已空。
  task_runners_.raster->PostTask([this, task = std::move(task)]() mutable {
    if (rasterizer_) {
      task(*rasterizer_);
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

void Shell::OnViewVisibilityChanged(platform::ViewId view, bool visible) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  PostToEngine([view, visible](Engine& engine) { engine.OnVisibilityChanged(view, visible); });
}

void Shell::OnPlatformViewFocused(int64_t view_id) {
  hub_->SendToJs("momo/platform_views",
                 EncodeMethodCall({"focused", EncodableValue(EncodableMap{{"id", view_id}})}));
}

void Shell::SetAppSuspended(bool suspended) {
  PostToEngine([suspended](Engine& engine) { engine.SetAppSuspended(suspended); });
}

// ---------------------------------------------------------------------------
// 平台视图通道(momo/platform_views, JS→平台):
//   create  {id: int, viewType: string, params: bytes|null}
//   dispose {id: int}
//   focus   {id: int}      把键盘焦点交给原生视图
//   blur    {id: int}      原生视图不再持有键盘焦点(若它正持有, 交还给视图自己的内容)
// 平台视图表归引擎(扩展经宿主能力表注册工厂, 合成帧按 LayerTree 中的位置切片), 所以这个通道由 Shell 处理;
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
  hub_->SetNativeHandler("momo/platform_views", task_runners_.platform,
                         [weak = weak_this_](MessageBytes message, MessageReply reply) {
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
  if (call.method == "blur") {
    for (const auto& [view, delegate] : delegates_) {
      delegate->OnPlatformViewBlurRequested(id);
    }
    return EncodableValue();
  }
  return UnimplementedError("momo/platform_views: unknown method " + call.method);
}

}  // namespace momo::engine
