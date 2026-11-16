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
//   从不等待 bundle 执行。JS 运行时在进程里只有这一个: Run 启动它, Shutdown 销毁它。
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
#include "engine/native_host/native_host.h"
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
    // 框架的焦点离开了视图: 若视图正持有键盘焦点, 交还给宿主视图自己的内容。
    virtual void OnPlatformViewBlurRequested(int64_t view_id) { (void)view_id; }

    // 本视图的第一帧已呈现到 surface(宿主可在此显示以 show:false 创建的窗口)。每次 AttachView 后最多一次。
    virtual void OnFirstFramePresented() {}

   protected:
    ~Delegate() = default;
  };

  // JS 运行时启动的结果(platform 线程): bundle 加载/执行失败时为错误(消息含 JS 栈)。
  using RunCallback = MoveOnlyFunction<void(Status)>;

  // 在 platform 线程调用。组装子系统, 不创建 JS 运行时、不执行 bundle, 不等待其他线程。返回后、Run 之前,
  // 宿主完成自己的准备(同 Flutter 桌面 embedder 在 Run 之前注册插件通道): 在 hub() 上注册通道处理器、
  // AttachView 绑定已有窗口——bundle 顶层代码因此看得到这些视图, 发出的通道调用也不会落空。
  // on_exit_requested: JS 调用 process.exit(code) 时在 platform 线程回调(此时 JS 已停止执行), 宿主据此做有序
  // 退出;为空时只停止 JS。
  static StatusOr<std::unique_ptr<Shell>> Create(
      const TaskRunners& task_runners, const Settings& settings,
      std::function<void(int exit_code)> on_exit_requested = nullptr);
  ~Shell() override;  // 隐式 Shutdown()

  // 启动 JS 运行时(只能调用一次): 在 ui 线程创建运行时并执行 bundle。立即返回;done(可空)在 bundle 的
  // 同步部分执行完后于 platform 线程回调(Shell 在此之前关闭则不回调)。失败时引擎没有运行时、不出帧,
  // 是否退出由宿主决定。
  void Run(RunCallback done = nullptr);

  // 帧时钟的来源(应用级, 与视图无关): 所有权移交 Shell, 之前的来源被释放(DeleteLater)。为空 = 按
  // settings.refresh_rate 的定时器(未调用时的默认)。跟随哪块显示器由宿主在来源内部决定。
  void SetVsyncSource(ObjectPtr<platform::VsyncSource> source);

  // 绑定视图: delegate 与 surface 必须在 DetachView/Shutdown 返回前保持有效。
  void AttachView(platform::ViewId view_id, Delegate* delegate, platform::RenderSurface* surface);
  // 解绑。返回后: delegate 不再被回调;raster 线程不再使用 surface(同步);
  // 视图从引擎移除(JS 收到 onViewsChanged)。
  void DetachView(platform::ViewId view_id);
  // 已绑定的视图(platform 线程)。
  std::vector<platform::ViewId> attached_views() const;

  // 有序关闭: 关闭宿主能力表 → 销毁全部平台视图(此后不再调用原生扩展)→ ui(销毁 JS 运行时, 再是 Engine 的
  // 其余部分)→ raster(释放 GPU 资源)。同步, 幂等。与 ui 线程汇合: 此前投递给 JS 的事件都先执行完;JS 不返回
  // (等待调试器连接、脚本死循环)时本调用就等着它——不强行终止 JS。
  void Shutdown();
  bool is_shut_down() const { return shut_down_; }

  // ---- 宿主事件入口(platform 线程调用, 转投 ui 线程) ----
  void OnMetricsChanged(platform::ViewId view, const platform::ViewportMetrics& metrics);
  void OnPointerPacket(platform::ViewId view, platform::PointerDataPacket packet);
  void OnKeyData(platform::ViewId view, const platform::KeyData& data);
  // 视图可见性(窗口显示/隐藏/最小化)。不可见的视图不出帧;全部不可见时停帧。
  void OnViewVisibilityChanged(platform::ViewId view, bool visible);
  // 平台视图获得了键盘焦点(用户点击等)→ momo/platform_views 通道 "focused"(native→JS)。
  void OnPlatformViewFocused(int64_t view_id);
  // 应用整体挂起(宿主隐藏了应用)时停帧, 恢复时继续。应用生命周期本身属于宿主。
  void SetAppSuspended(bool suspended);

  const TaskRunners& task_runners() const { return task_runners_; }
  // 消息通道(任意线程可用;Shell 关闭后仍可安全调用, 只是不再有 handler)。
  const std::shared_ptr<MessageChannelHub>& hub() const { return hub_; }
  const std::shared_ptr<graphics::TextureRegistry>& textures() const { return textures_; }
  // 平台视图表(platform 线程)。
  PlatformViewRegistry& platform_views() { return *platform_views_; }

 private:
  Shell(const TaskRunners& task_runners, const Settings& settings);

  // platform 线程 → ui 线程, Engine 存在时执行。
  void PostToEngine(MoveOnlyFunction<void(Engine&)> task);
  Delegate* DelegateOf(platform::ViewId view) const;
  // source 未绑定时为回退定时器。
  std::unique_ptr<VsyncWaiter> MakeVsyncWaiter(WeakPtr<platform::VsyncSource> source) const;

  // 引擎自己的通道 momo/platform_views(platform 线程处理)。
  void RegisterPlatformViewChannel();
  StatusOr<EncodableValue> HandlePlatformViewCall(const MethodCall& call);

  // ui 线程(Engine 的 on_draw): 把新帧投递给 raster。
  void OnEngineDraw(std::shared_ptr<LayerTreePipeline> pipeline);

  const TaskRunners task_runners_;
  const double refresh_rate_;
  const std::shared_ptr<MessageChannelHub> hub_;
  const std::shared_ptr<graphics::TextureRegistry> textures_;
  // 与 NativeHost 共享(扩展注册工厂), 实例的创建/销毁只经 Shell(platform 线程)。
  const std::shared_ptr<PlatformViewRegistry> platform_views_;
  // 宿主能力表(原生扩展经 momo:services 的 nativeHost 取得)。
  NativeHost native_host_;
  bool shut_down_ = false;
  bool run_called_ = false;  // 仅 platform 线程

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
