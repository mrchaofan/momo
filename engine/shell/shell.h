#pragma once

// Shell: 引擎实例的协调中枢与生命周期根对象(docs/02 §2)。
// 线程约定: momo::Object, 归属 platform 线程: 在该线程创建、调用与析构;内部把工作投递到 ui/raster 线程。
// Shell 不拥有线程(线程由 embedder 创建与 join, docs/01 §4)。
//
// 子系统与归属线程:
//   platform: 宿主视图 Delegate、帧时钟的 VsyncSource(ObjectPtr 持有, ui 只拿 WeakPtr)、平台视图表
//   ui:       Engine(Animator + JS 运行时/AppDriver)
//   raster:   Rasterizer(surface 与 GPU 资源)
// 两阶段启动: Create 只组装子系统(没有 JS 运行时), 同步返回;宿主随后注册通道处理器、添加进程内扩展、
//   绑定视图, 再调用 Run 异步启动 JS 运行时(ui 线程执行 bundle, 结果回到 platform 线程)。platform 线程
//   从不等待 bundle 执行。首次启动(Run)与 hot restart(RequestHotRestart)走同一条"启动运行时"的链路。
// 帧路径: Engine(ui)产出 FrameItem(各视图的 LayerTree)→ Pipeline(深度 2)→ raster 线程 Rasterizer::Draw。
// 多视图: 每个视图(窗口)以 AttachView(view_id, …) 绑定自己的 Delegate 与 surface。帧时钟是应用级的
//   (所有视图共用一个 Animator): 宿主以 SetVsyncSource 给出唯一的 VsyncSource, 没有时用定时器。视图的
//   创建与关闭都由宿主决定(宿主分配 id 后 AttachView / DetachView), 引擎只认识视图本身。
// 职责边界: Shell 只承载引擎职责(画面、输入、视图尺寸与焦点、平台视图合成、扩展、消息通道)。
//   窗口外观与行为、剪贴板、打开 URL、输入法等平台能力都由 embedder 在 hub() 上注册通道处理器,
//   不经过 Shell——新增这类能力不需要改引擎。

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/closure.h"
#include "base/macros.h"
#include "base/object.h"
#include "base/status.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/platform_view_registry.h"
#include "engine/common/standard_codec.h"
#include "engine/common/settings.h"
#include "engine/common/task_runners.h"
#include "engine/extension/extension_host.h"
#include "engine/graphics/texture.h"
#include "engine/platform/input_types.h"
#include "engine/platform/render_surface.h"
#include "engine/platform/view.h"
#include "engine/platform/viewport_metrics.h"
#include "engine/platform/vsync_source.h"
#include "engine/shell/engine.h"

namespace momo::engine {

class Rasterizer;

class Shell final : public Object {
 public:
  // 引擎 → 宿主视图的请求, 在 platform 线程回调。由 embedder 的 WindowController 实现。
  class Delegate {
   public:
    // 本视图的原生句柄(NSView* / HWND), 供扩展 get_window_handle 使用。无窗口时返回空。
    virtual void* GetNativeWindowHandle() = 0;

    // ---- 平台视图(docs/05 §9;默认空实现 = 宿主不嵌入平台视图) ----
    // 平台视图表是全局的: 创建/销毁/聚焦请求广播给所有窗口。原生视图实际挂在哪个窗口由合成帧决定
    // (哪个窗口的画面里出现它, 就由那个窗口接管)。
    // 视图已创建(或窗口刚绑定, 补发已有视图): 登记(初始不显示)。
    virtual void OnPlatformViewCreated(int64_t view_id, void* native_view) {
      (void)view_id;
      (void)native_view;
    }
    // 视图即将被扩展销毁(或宿主解绑): 移出窗口并释放宿主持有的引用。
    virtual void OnPlatformViewDisposed(int64_t view_id) { (void)view_id; }
    // 框架请求把键盘焦点交给视图(macOS: makeFirstResponder)。
    virtual void OnPlatformViewFocusRequested(int64_t view_id) { (void)view_id; }

    // 本视图的第一帧已呈现到 surface(宿主可在此显示以 show:false 创建的窗口)。每次 AttachView 后最多一次。
    virtual void OnFirstFramePresented() {}

   protected:
    ~Delegate() = default;
  };

  // JS 运行时启动的结果(platform 线程): bundle 加载/执行失败时为错误(消息含 JS 栈)。
  using RunCallback = MoveOnlyFunction<void(Status)>;

  // 在 platform 线程调用。组装子系统并加载 manifest 声明的扩展(on_attach), 不创建 JS 运行时、不执行
  // bundle, 不等待其他线程。返回后、Run 之前, 宿主完成自己的准备(同 Flutter 桌面 embedder 在 Run 之前
  // 注册插件通道): 在 hub() 上注册通道处理器、extensions().AddExtension 添加进程内扩展、AttachView 绑定
  // 已有窗口——bundle 顶层代码因此看得到这些视图, 发出的通道调用也不会落空。
  // on_exit_requested: JS 调用 process.exit(code) 时在 platform 线程回调(此时 JS 已停止执行), 宿主据此做有序
  // 退出;为空时只停止 JS。
  static StatusOr<std::unique_ptr<Shell>> Create(
      const TaskRunners& task_runners, const Settings& settings,
      std::function<void(int exit_code)> on_exit_requested = nullptr);
  ~Shell() override;  // 隐式 Shutdown()

  // 启动 JS 运行时(只能调用一次): 向扩展派发 RuntimeStarting, 然后在 ui 线程创建运行时并执行 bundle。
  // 立即返回;done(可空)在 bundle 的同步部分执行完后于 platform 线程回调(Shell 在此之前关闭则不回调)。
  // 失败时引擎退回占位驱动, 是否退出由宿主决定。
  void Run(RunCallback done = nullptr);

  // 帧时钟的来源(应用级, 与视图无关): 所有权移交 Shell, 之前的来源被释放(DeleteLater)。为空 = 按
  // settings.refresh_rate 的定时器(未调用时的默认)。跟随哪块显示器由宿主在来源内部决定。
  void SetVsyncSource(ObjectPtr<platform::VsyncSource> source);

  // 绑定视图: delegate 与 surface 必须在 DetachView/Shutdown 返回前保持有效。
  void AttachView(platform::ViewId view_id, Delegate* delegate, platform::RenderSurface* surface);
  // 解绑。返回后: delegate 不再被回调;raster 线程不再使用 surface(同步);
  // 视图从引擎移除(JS 收到 onViewsChanged)。
  void DetachView(platform::ViewId view_id);
  // 隐式视图(视图 0)的便捷入口。
  void AttachView(Delegate* delegate, platform::RenderSurface* surface) {
    AttachView(platform::kImplicitViewId, delegate, surface);
  }
  void DetachView() { DetachView(platform::kImplicitViewId); }
  // 已绑定的视图(platform 线程)。
  std::vector<platform::ViewId> attached_views() const;

  // 有序关闭: 停止 JS 运行时(同 hot restart 的拆除顺序)→ ui(销毁 Engine)→ 扩展 on_detach →
  // raster(释放 GPU 资源)。同步, 幂等。
  void Shutdown();
  bool is_shut_down() const { return shut_down_; }

  // ---- 宿主事件入口(platform 线程调用, 转投 ui 线程) ----
  void OnMetricsChanged(platform::ViewId view, const platform::ViewportMetrics& metrics);
  void OnPointerPacket(platform::ViewId view, platform::PointerDataPacket packet);
  void OnKeyData(platform::ViewId view, const platform::KeyData& data);
  void OnFocusChanged(platform::ViewId view, bool focused);
  // 视图可见性(窗口显示/隐藏/最小化)。不可见的视图不出帧;全部不可见时停帧。
  void OnViewVisibilityChanged(platform::ViewId view, bool visible);
  // 隐式视图(视图 0)的便捷入口。
  void OnMetricsChanged(const platform::ViewportMetrics& metrics) {
    OnMetricsChanged(platform::kImplicitViewId, metrics);
  }
  void OnPointerPacket(platform::PointerDataPacket packet) {
    OnPointerPacket(platform::kImplicitViewId, std::move(packet));
  }
  void OnKeyData(const platform::KeyData& data) { OnKeyData(platform::kImplicitViewId, data); }
  void OnFocusChanged(bool focused) { OnFocusChanged(platform::kImplicitViewId, focused); }
  // 平台视图获得了键盘焦点(用户点击等)→ momo/platform_views 通道 "focused"(native→JS)。
  void OnPlatformViewFocused(int64_t view_id);
  // 重新加载 bundle(实验性): 停止当前 JS 运行时(同步)再启动一个新的(异步, 与 Run 同一条链路;视图、
  // 扩展、通道处理器都保留)。旧运行时创建的平台视图随之销毁;旧 JS 打开的窗口由宿主自行决定是否关闭
  // (窗口归宿主)。Run 之前或关闭之后调用无效。done 同 Run。
  void RequestHotRestart(RunCallback done = nullptr);
  // 应用整体挂起(宿主隐藏了应用)时停帧, 恢复时继续。应用生命周期本身属于宿主。
  void SetAppSuspended(bool suspended);

  const TaskRunners& task_runners() const { return task_runners_; }
  // 消息通道(任意线程可用;Shell 关闭后仍可安全调用, 只是不再有 handler)。
  const std::shared_ptr<MessageChannelHub>& hub() const { return hub_; }
  // 扩展宿主(platform 线程)。Create 时按 settings.extensions_dir 的 manifest 加载;进程内扩展
  // (静态链接/测试)在 Run 之前以 extensions().AddExtension(id, init) 添加。
  ExtensionHost& extensions() { return *extensions_; }
  const std::shared_ptr<graphics::TextureRegistry>& textures() const { return textures_; }
  // 平台视图表(platform 线程)。
  PlatformViewRegistry& platform_views() { return *platform_views_; }

 private:
  Shell(const TaskRunners& task_runners, const Settings& settings);

  // "启动/停止 JS 运行时"的唯一链路(platform 线程)。首次启动 = Start;hot restart = Stop + Start;
  // 关闭 = Stop。运行时的代数只在这里推进(runtime_generation_), 扩展的生命周期事件、消息通道的代际、
  // Promise token 都取自它。
  //   Start: RuntimeStarting(#n) → ui: Engine::StartRuntime(n)(异步;结果经 done 回到 platform)。
  //   Stop:  ui: Engine::StopRuntime(同步汇合)→ 销毁全部平台视图 → RuntimeStopped(#n)。
  void StartRuntime(MomoLifecycleReason reason, RunCallback done);
  void StopRuntime(MomoLifecycleReason reason);

  // platform 线程 → ui 线程, Engine 存在时执行。
  void PostToEngine(MoveOnlyFunction<void(Engine&)> task);
  // 任意线程 → platform 线程, Shell 未关闭且视图已绑定时执行。
  void PostToDelegate(platform::ViewId view, MoveOnlyFunction<void(Delegate&)> task);
  Delegate* DelegateOf(platform::ViewId view) const;
  // source 未绑定时为回退定时器。
  std::unique_ptr<VsyncWaiter> MakeVsyncWaiter(WeakPtr<platform::VsyncSource> source) const;

  // 销毁全部平台视图(StopRuntime;platform 线程): 先通知窗口移除, 再交给扩展释放。
  void DisposeAllPlatformViews();
  // 引擎自己的通道: momo/platform_views、momo/extensions(platform 线程处理)。
  void RegisterEngineChannels();
  StatusOr<EncodableValue> HandlePlatformViewCall(const MethodCall& call);
  StatusOr<EncodableValue> HandleExtensionsCall(const MethodCall& call);

  // ui 线程(Engine 的 on_draw): 把新帧投递给 raster。
  void OnEngineDraw(std::shared_ptr<LayerTreePipeline> pipeline);

  const TaskRunners task_runners_;
  const double refresh_rate_;
  const std::shared_ptr<MessageChannelHub> hub_;
  const std::shared_ptr<graphics::TextureRegistry> textures_;
  // platform 线程。与 ExtensionHost 共享(扩展注册工厂), 实例的创建/销毁只经 Shell。
  const std::shared_ptr<PlatformViewRegistry> platform_views_;
  // platform 线程。先于 Engine 创建(on_attach 注册的模块在 JS 运行时创建时安装), 在 Engine 销毁后 detach。
  std::unique_ptr<ExtensionHost> extensions_;
  bool shut_down_ = false;
  // 仅 platform 线程。JS 运行时的代数: 首个为 1, 每次 StartRuntime 加一(唯一的真相源)。
  uint64_t runtime_generation_ = 0;
  // 已派发 RuntimeStarting、尚未派发对应的 RuntimeStopped。
  bool runtime_started_ = false;

  // 仅 platform 线程访问。
  std::map<platform::ViewId, Delegate*> delegates_;
  ObjectPtr<platform::VsyncSource> vsync_source_;  // 帧时钟的来源(空 = 定时器)
  // 仅 ui 线程访问。
  std::unique_ptr<Engine> engine_;
  // 仅 raster 线程访问。
  std::unique_ptr<Rasterizer> rasterizer_;

  // 构造时取一次, 之后只读: 任意线程可拷贝(只在 platform 线程解引用)。
  // Shutdown 时 InvalidateWeakPtrs 使其失效, 拦住关闭后才到达的 platform 回调。
  const WeakPtr<Shell> weak_this_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Shell);
};

}  // namespace momo::engine
