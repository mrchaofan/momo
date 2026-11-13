#pragma once

// Shell: 引擎实例的协调中枢与生命周期根对象(docs/02 §2)。
// 线程约定: momo::Object, 归属 platform 线程: 在该线程创建、调用与析构;内部把工作投递到 ui/raster 线程。
// Shell 不拥有线程(线程由 embedder 创建与 join, docs/01 §4)。
//
// 子系统与归属线程:
//   platform: 宿主视图 Delegate、视图的 VsyncSource(ObjectPtr 持有, ui 只拿 WeakPtr)、平台视图表
//   ui:       Engine(AppDriver + Animator)
//   raster:   Rasterizer(surface 与 GPU 资源)
// 帧路径: Engine(ui)产出 FrameItem(各视图的 LayerTree)→ Pipeline(深度 2)→ raster 线程 Rasterizer::Draw。
// 多视图: 每个视图(窗口)以 AttachView(view_id, …) 绑定自己的 Delegate、surface 与 VsyncSource;
//   帧时钟只用一个视图的 VsyncSource(优先视图 0)。视图的创建与关闭都由宿主决定(宿主分配 id 后
//   AttachView / DetachView), 引擎只认识视图本身。
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

class Shell final : public Object, private Engine::Delegate {
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

  // 在 platform 线程调用;同步等待 raster 与 ui 两侧初始化完成(期间执行 JS bundle)。
  // on_created: 在 JS bundle 执行之前于 platform 线程调用(此时 hub() 已可用): 宿主在这里注册自己的
  // 通道处理器(窗口、平台服务), bundle 顶层代码发出的调用因此不会落空(同 Flutter 桌面 embedder 在
  // Run 之前注册插件通道)。处理器在 Create 返回后才会在 platform 线程上运行。
  // on_exit_requested: JS 调用 process.exit(code) 时在 platform 线程回调(此时 JS 已停止执行), 宿主据此做有序
  // 退出;为空时只停止 JS。
  // in_process_extensions: 额外加载的进程内扩展(静态链接/测试), (id, init) 对。
  static StatusOr<std::unique_ptr<Shell>> Create(
      const TaskRunners& task_runners, const Settings& settings,
      const std::function<void(Shell&)>& on_created = nullptr,
      std::function<void(int exit_code)> on_exit_requested = nullptr,
      std::vector<std::pair<std::string, MomoExtInitFn>> in_process_extensions = {});
  ~Shell() override;  // 隐式 Shutdown()

  // 绑定视图: delegate 与 surface 必须在 DetachView/Shutdown 返回前保持有效。
  // vsync 为该窗口的 VsyncSource(可为空), 所有权移交 Shell;帧时钟优先用视图 0 的, 都没有时回退定时器。
  void AttachView(platform::ViewId view_id, Delegate* delegate, platform::RenderSurface* surface,
                  ObjectPtr<platform::VsyncSource> vsync = nullptr);
  // 解绑。返回后: delegate 不再被回调;raster 线程不再使用 surface(同步);
  // VsyncSource 已释放(DeleteLater);视图从引擎移除(JS 收到 onViewsChanged)。
  void DetachView(platform::ViewId view_id);
  // 隐式视图(视图 0)的便捷入口。
  void AttachView(Delegate* delegate, platform::RenderSurface* surface,
                  ObjectPtr<platform::VsyncSource> vsync = nullptr) {
    AttachView(platform::kImplicitViewId, delegate, surface, std::move(vsync));
  }
  void DetachView() { DetachView(platform::kImplicitViewId); }
  // 已绑定的视图(platform 线程)。
  std::vector<platform::ViewId> attached_views() const;

  // 有序关闭: ui(停帧、销毁 JS 运行时)→ raster(释放 GPU 资源)。同步, 幂等。
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
  // --dev: 重新加载 bundle(销毁并重建 JS 运行时, 保留 isolate 之外的一切状态)。旧运行时创建的
  // 平台视图随之销毁;旧 JS 打开的窗口由宿主自行决定是否关闭(窗口归宿主)。
  void RequestHotRestart();
  // 应用整体挂起(宿主隐藏了应用)时停帧, 恢复时继续。应用生命周期本身属于宿主。
  void SetAppSuspended(bool suspended);

  const TaskRunners& task_runners() const { return task_runners_; }
  // 消息通道(任意线程可用;Shell 关闭后仍可安全调用, 只是不再有 handler)。
  const std::shared_ptr<MessageChannelHub>& hub() const { return hub_; }
  // 扩展宿主(platform 线程)。Create 时按 settings.extensions_dir 的 manifest 加载。
  ExtensionHost& extensions() { return *extensions_; }
  const std::shared_ptr<graphics::TextureRegistry>& textures() const { return textures_; }
  // 平台视图表(platform 线程)。
  PlatformViewRegistry& platform_views() { return *platform_views_; }

 private:
  Shell(const TaskRunners& task_runners, const Settings& settings);

  // platform 线程 → ui 线程, Engine 存在时执行。
  void PostToEngine(MoveOnlyFunction<void(Engine&)> task);
  // 任意线程 → platform 线程, Shell 未关闭且视图已绑定时执行。
  void PostToDelegate(platform::ViewId view, MoveOnlyFunction<void(Delegate&)> task);
  Delegate* DelegateOf(platform::ViewId view) const;
  // 重新选择驱动帧时钟的 VsyncSource(视图 0 优先, 其次任一已绑定视图;都没有时为定时器)。
  void UpdateVsyncSource();
  // source 未绑定时为回退定时器。
  std::unique_ptr<VsyncWaiter> MakeVsyncWaiter(WeakPtr<platform::VsyncSource> source) const;

  // 销毁全部平台视图(热重启、关闭;platform 线程): 先通知窗口移除, 再交给扩展释放。
  void DisposeAllPlatformViews();
  void RegisterPlatformViewChannel();
  StatusOr<EncodableValue> HandlePlatformViewCall(const MethodCall& call);

  // Engine::Delegate(ui 线程)。
  void OnEngineDraw(std::shared_ptr<LayerTreePipeline> pipeline) override;

  const TaskRunners task_runners_;
  const double refresh_rate_;
  const std::shared_ptr<MessageChannelHub> hub_;
  const std::shared_ptr<graphics::TextureRegistry> textures_;
  // platform 线程。与 ExtensionHost 共享(扩展注册工厂), 实例的创建/销毁只经 Shell。
  const std::shared_ptr<PlatformViewRegistry> platform_views_;
  // platform 线程。先于 Engine 创建(on_attach 注册的模块在 JS 运行时创建时安装), 在 Engine 销毁后 detach。
  std::unique_ptr<ExtensionHost> extensions_;
  bool shut_down_ = false;

  // 仅 platform 线程访问。
  std::map<platform::ViewId, Delegate*> delegates_;
  std::map<platform::ViewId, ObjectPtr<platform::VsyncSource>> vsync_sources_;
  platform::VsyncSource* active_vsync_ = nullptr;  // 当前驱动帧时钟的来源(vsync_sources_ 之一)
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
