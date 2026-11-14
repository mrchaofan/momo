# 03 · 平台契约与平台实现

本文回答"框架如何在这个操作系统上运行与显示":窗口、渲染表面、输入、IME、剪贴板、光标、菜单、对话框、VSync。
它不是独立的一层,而是 engine 与 embedder 之间的边界,分三部分(对标 Flutter:engine 内的 `Surface`/`VsyncWaiter`/`PlatformView` 接口 + `shell/platform/<os>` 实现):

1. **平台契约**(`engine/platform/`,命名空间 `momo::engine::platform`):engine 对宿主的全部要求,纯虚接口 + POD。
   接口归使用方 engine 所有(依赖倒置),engine 只认识它们,因此可脱离真实平台做 headless 测试。
2. **embedder 的平台抽象**(`embedder/common/`,命名空间 `momo::embedder`):窗口 `PlatformWindow`、工厂 `PlatformIntegration`、`DisplayInfo`。
   只有 embedder 的装配逻辑(Application/WindowController)使用,engine 不可见;它让 `embedder/common` 平台无关、可 mock 单测。
3. **平台实现**(`embedder/macos/`、`embedder/windows/`):实现上述两组接口,与各自的进程入口放在一起。

移植新平台 = 在 `embedder/<os>/` 实现这两组接口 + 写入口。

## 1. 目录结构

```
engine/platform/                   # 平台契约(engine 可见的全部;零平台 include、零三方依赖, 只依赖 base)
├── render_surface.h               # 渲染表面接口(GPU/软件)、SurfaceFrame、GpuContextInfo
├── vsync_source.h                 # VSync 信号源(momo::Object, 归属 platform 线程)
├── input_types.h                  # PointerData/KeyData 等平台无关事件类型
├── view.h                         # ViewId(视图 0 = 隐式视图)
├── platform_view.h                # 平台视图的合成布局(raster 产出, embedder 消费)
└── viewport_metrics.h             # ViewportMetrics(物理尺寸 + DPR)

embedder/common/                   # embedder 的平台抽象(engine 不可见)
├── platform_integration.h         # 工厂与根入口(创建窗口/VsyncSource、服务、应用能力、显示器、原生循环)
├── platform_app.h                 # 应用级能力(隐藏/显示/激活, AppCapability)
├── platform_window.h              # 窗口接口(含可选能力 WindowCapability)+ WindowDelegate 回调 + WindowSpec
├── platform_services.h            # 剪贴板/打开 URL(低频服务)
├── text_input.h                   # IME 接入接口与 TextEditingState/TextInputEvent
├── window_types.h                 # SystemCursor、WindowOptions、WindowStateFlags、TitleBarOptions、WindowResizeOptions…
├── host_channel.h                 # 宿主通道的公共实现(方法注册表、宿主 → JS 通知与询问)
├── app_channel.h                  # 宿主通道 momo/app(应用生命周期与退出)
├── window_channel.h               # 宿主通道 momo/window(窗口)
├── platform_channels.h            # 宿主通道 momo/clipboard、momo/url、momo/textinput
├── pointer_dispatcher.h           # 指针接收方的唯一决策者(PointerDispatcher)+ 平台窗口执行端 PointerWindow
├── display.h                      # 显示器信息(DPR、刷新率、工作区;主屏左上原点的逻辑点)
└── (menu.h)                       # 应用菜单与右键菜单接口(规划)

engine/platform/ 只放 engine 自己要用的东西(画面、输入、视图尺寸)。窗口外观与行为、剪贴板、输入法这类能力
engine 不需要知道: 它们由 embedder 在引擎的消息通道上注册处理器(宿主通道), JS 与宿主直接往返。
(原生消息循环接口 NativeLoop 定义在 base/native_loop.h, 见 docs/12)

embedder/windows/                  # Win32 实现(与 main_win.cc、win_app_delegate 同目录)
├── win_platform_integration.h/.cc # 工厂: 持有 WinMouseInput、WinDisplays、WinApp(同 macOS)
├── win_app.h/.cc                  # PlatformApp(Activate)+ WM_ACTIVATEAPP 汇集(WinAppObserver)
├── win_native_loop.h/.cc          # NativeLoop: 隐藏消息窗口 PostMessage + 自有消息泵
├── win_window.h/.cc               # HWND 封装、WndProc、PointerWindow、窗口框架、DPI per-monitor v2
├── win_mouse_events.h/.cc         # WinMouseInput: 鼠标消息 → PointerInput → PointerDispatcher(纯 C++, 可单测)
├── win_frame.h/.cc                # 内侧缩放边框、标题栏按钮布局与命中(纯 C++, 可单测)
├── win_caption_buttons.h/.cc      # 隐藏标题栏时宿主绘制的最小化/最大化/关闭按钮(分层子窗口)
├── win_screen_layout.h/.cc        # 多 DPI 显示器的物理像素 ↔ 全局逻辑点(纯 C++, 可单测)
├── win_displays.h/.cc             # 显示器枚举与布局缓存
├── win_surface_d3d.h/.cc          # D3D12 交换链 surface(可回退软件)
├── win_surface_software.h/.cc     # GDI 软件 surface
├── win_vsync_dwm.h/.cc            # DwmFlush vblank 等待
├── win_text_input.h/.cc           # IMM32
├── win_services.h/.cc             # 剪贴板、OpenUrl
└── win_key_mapping / win_util

embedder/macos/                    # Cocoa 实现(Objective-C++)
├── main_mac.mm                    # 进程入口(只做翻译)
├── mac_app_delegate.h/.mm         # NSApplicationDelegate: 退出拦截、隐藏/激活 → Application;最小主菜单
├── mac_platform_integration.h/.mm # PlatformIntegration 工厂 + PlatformApp + PlatformServices;持有 MacMouseInput
├── mac_native_loop.h/.mm          # NativeLoop: GCD 主队列 + NSApp run/stop
├── mac_window.h/.mm               # MacWindow: NSWindow/NSPanel + 内容视图 MomoContentView(键盘、IME、光标)
├── mac_mouse_events.h/.mm         # MacMouseInput(进程内一个: NSEvent → PointerInput → PointerDispatcher)、
│                                  # 事件分类、跟踪区工具、屏幕坐标换算
├── mac_key_mapping.h/.mm          # 键盘事件 → KeyData(keycode → HID 全表、逻辑键、修饰键)
├── mac_platform_views.h/.mm       # 平台视图宿主(几何: 裁剪、圆角、透明度、z 序)
├── mac_platform_view_input.h/.mm  # 平台视图的输入投递(派发器决定接收方后经 AppKit 投递)
├── mac_surface_metal.h/.mm        # CAMetalLayer surface(含平台视图合成、拖拽缩放同步)
├── mac_surface_software.h/.mm     # IOSurface 软件 surface(Metal 失败时回退)
├── mac_vsync_displaylink.h/.mm    # CVDisplayLink
└── mac_text_input.h/.mm           # IME 状态(NSTextInputClient 的同步查询)
```

规则:`engine/platform/` 头文件零平台 include、零三方依赖(可含 `base/` 的几何、Status、`momo::Object` 等);
平台对象若有线程归属(如 `VsyncSource`)继承 `momo::Object`,以 `ObjectPtr` 交出所有权(docs/08 §3)。
embedder 内以 `platform::X` 指代契约类型(`embedder/common/engine_platform.h` 的命名空间别名)。

## 2. 核心接口草案

```cpp
// embedder/common/platform_integration.h
class PlatformIntegration {
 public:
  virtual ~PlatformIntegration() = default;
  virtual StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(const WindowSpec&, WindowDelegate*) = 0;
  virtual StatusOr<ObjectPtr<VsyncSource>> CreateVsyncSource(const PlatformWindow&) = 0;  // momo::Object, 归属 platform 线程
  virtual PlatformServices& services() = 0;  // embedder 抽象(platform_services.h)
  virtual PlatformApp& app() = 0;            // 应用级能力(platform_app.h)
  virtual std::vector<DisplayInfo> displays() const = 0;
  virtual NativeLoop& native_loop() = 0;  // OS 主线程原生消息循环(embedder 在其上跑 EventLoop)
};

// base/native_loop.h(接口在 base, 实现在平台层)
class NativeLoop {
 public:
  virtual void PostTask(Closure task) = 0;                       // 任意线程
  virtual void PostDelayedTask(Closure task, TimeDelta delay) = 0;
  virtual bool RunsTasksOnCurrentThread() const = 0;             // Run 之前也须正确
  virtual void Run() = 0;                                        // 仅 OS 主线程, 阻塞至 Quit, 可嵌套
  virtual void Quit() = 0;                                       // 结束最内层
};

// embedder/common/platform_window.h
class PlatformWindow {
 public:
  virtual ~PlatformWindow() = default;
  virtual void Show() = 0;
  virtual void SetTitle(std::string_view) = 0;
  virtual void SetMinSize(Size) = 0;
  virtual Size physical_size() const = 0;
  virtual double device_pixel_ratio() const = 0;
  virtual RenderSurface& surface() = 0;
  virtual void SetCursor(SystemCursor) = 0;
  virtual TextInputConnection& text_input() = 0;
  virtual void* native_handle() const = 0;   // HWND / NSView*, 供扩展使用(docs/07)
};

// 事件走回调(platform 线程调用, 由 embedder 的 WindowController 转投 ui 线程)
class WindowDelegate {
 public:
  virtual void OnMetricsChanged(const ViewportMetrics&) = 0;
  virtual void OnPointerPacket(PointerDataPacket) = 0;
  virtual void OnKeyData(const KeyData&) = 0;
  virtual bool OnCloseRequested() = 0;   // 返回 false 拦截关闭
  virtual void OnFocusChanged(bool focused) = 0;
  virtual void OnWindowStateChanged(const WindowStateFlags&) = 0;  // 显示/隐藏、最小化、最大化、全屏 → 视图可见性
 protected:
  ~WindowDelegate() = default;
};

// engine/platform/render_surface.h — raster 线程使用
class RenderSurface {
 public:
  virtual ~RenderSurface() = default;
  virtual SurfaceKind kind() const = 0;            // kMetal | kDirect3D | kSoftware
  virtual StatusOr<SurfaceFrame> AcquireFrame(Size physical_size) = 0;
  // SurfaceFrame{kind, software: SoftwarePixels{pixels,row_bytes,width,height}(BGRA 预乘), gpu_target(M2), present}
};
```

## 3. 输入事件归一化

`input_types.h` 定义平台无关事件,在各实现内完成翻译:

- 指针:坐标统一为**物理像素**、按钮位图、`PointerPhase{add,down,move,up,remove(离开),hover,scroll,gesture,cancel(序列被取消)}`、按下序列 id(`sequence_id`);鼠标滚轮/触控板精细滚动统一为 scroll delta(px);Mac 的惯性滚动阶段保留 momentum 标记。每个事件另带屏幕位置(逻辑点, 主屏左上原点, 与 screen_frame 同一坐标系;screen_frame 与 SetPosition 均以内容区为准, 屏幕坐标 − 框原点即窗口内坐标), 供跨窗口交互(如标签拖出成窗口、拖动中移动窗口本身)使用, 不受窗口自身移动影响。指针序列的接收方是固定的(拖动一旦开始, 不随指针经过的窗口变化): 宿主决定原生接收窗口——按下时隐式是按下所在的窗口, 进程内唯一, 序列期间别的窗口不上报这个指针;`momo/window capturePointer` 改交(拖出成窗、并入另一个窗口), 不合成事件;按下所在的窗口关闭后序列照常继续(macOS: 进程级本地事件监视器统一路由;Windows: `::SetCapture`);原生接收窗口被关闭时以取消结束(`PlatformWindow::ReleasePointerCapture`)。框架决定逻辑接收方(`GestureBinding.capturePointer`)。引擎不参与。详见 ARCHITECTURE §3.10"指针序列与捕获"。
- 键盘:三元组(physical key = USB HID usage,logical key,字符);修饰键状态快照。文本输入不走 KeyData,而走 IME 通道。
- IME:`TextInputConnection` 实现双向协议——JS 侧 EditingState(文本、选区、composing 区间)同步给平台;平台回传 commit/composing 更新;候选窗位置由 JS 框架上报光标矩形。

## 4. VSync 与显示

`VsyncSource::RequestFrameCallback(cb)` 一次性请求,由 `Animator` 按需发起(无动画不请求,空闲零功耗)。`VsyncSource` 是 platform 线程归属的 `momo::Object`:请求经投递在 platform 线程调用,回调也在 platform 线程执行(回调一律在调用方线程执行;CVDisplayLink / DwmFlush 线程只投递, 不调用回调);由 `ObjectPtr` 持有,释放即 DeleteLater,析构可能晚于窗口销毁,实现不得依赖窗口对象存活(docs/08 §3)。Windows 用 DWM/DXGI 等待 vblank 的专用线程;macOS 用 CVDisplayLink。DPR 或刷新率变化(拖拽跨屏)通过 `OnMetricsChanged` 上报。

## 5. 平台服务与消息通道

应用生命周期、窗口管理、剪贴板、系统对话框、菜单、打开 URL 等低频能力不做同步绑定,统一经 `MessageChannelHub` 以异步消息暴露。处理器由 **embedder** 注册(`app_channel.cc`:`momo/app`;`window_channel.cc`:`momo/window`;`platform_channels.cc`:`momo/clipboard`、`momo/url`、`momo/textinput`), 调用 `PlatformApp` / `PlatformWindow` / `PlatformServices`;注册发生在 `Shell::Create` 的 `on_created` 回调里, 早于 JS bundle 执行。新增服务、窗口或应用能力 = embedder 里注册一个通道方法(`HostChannel::Register`)+ 框架对象(`app` / `Widget`)加方法, 不改 engine, 不改 ui 绑定层, 这与扩展机制使用同一套通路(docs/07)。

窗口的可选能力(隐藏标题栏、系统拖动、移动循环、指针捕获、置顶、定位、平台视图、隐藏、最小化/最大化…)由 `PlatformWindow::capabilities()` 显式声明, 不支持的方法返回 `Unimplemented`(JS 收到错误码 `unimplemented`, 框架当作空操作), JS 可经 `momo/window getState` 查询。

## 6. 实现要点备忘

- **Windows**(未在 Windows 上运行验证(macOS 上: 纯逻辑部分有单测, 依赖 Win32 的部分经 mingw-w64 交叉编译检查)):
  - 指针: 与 macOS 同一套路由——`WinMouseInput`(进程内一个, 持有 `PointerDispatcher`)翻译鼠标消息, `WinWindow` 作为
    `PointerWindow` 执行;本窗口是进行中 momo 序列的接收方时持有 `SetCapture`, `capturePointer` 改交时捕获随之转移。
    被系统模态循环(系统拖动、菜单)吞掉的抬起由派发器的过期判定补报。
  - 屏幕坐标: 全局逻辑点(主屏左上原点)按 Chromium 的做法布局混合 DPI 显示器(`WinScreenLayout`), 指针、`screen_frame`、
    `SetPosition`、`displays()` 共用。
  - 窗口框架: 内侧缩放边框经 `WM_NCHITTEST` 归系统;隐藏标题栏 = `WM_NCCALCSIZE` 让内容铺满窗口(最大化时内缩一圈边框),
    系统不再画控制按钮, 由宿主绘制(`WinCaptionButtons`, 命中返回 `HTMINBUTTON`/`HTMAXBUTTON`/`HTCLOSE`, Windows 11 贴靠布局可用),
    占用区域经 `OnWindowControlsChanged` 报告——应用侧与 macOS 的红绿灯一样只需避让(框架 `MediaQueryData.windowControlsPadding`)。
    系统拖动 = `WM_NCLBUTTONDOWN(HTCAPTION)`(含 Aero Snap), 双击最大化/还原。
  - 应用生命周期: `WM_ACTIVATEAPP` → `WinApp` 去重 → 入口的 `WinAppDelegate` → `Application::OnAppActiveChanged`;再次启动应用
    (单实例转发)= macOS 点击 Dock 图标(激活 + `OnReopen`);`WM_ENDSESSION` 有序关闭。没有应用级隐藏。
  - 渲染: D3D12 flip-model 交换链(`WinD3DSurface`, raster 线程 `ResizeBuffers`/`Present`), 失败回退 GDI;拖拽缩放时 `WM_SIZE`
    里等待一帧新尺寸的帧(同 macOS, 上限 100 ms)。深色标题栏 DWMWA_USE_IMMERSIVE_DARK_MODE;WM_DPICHANGED。
  - 不支持: 平台视图、非激活窗口(`non_activating` 按普通窗口处理)。
- **macOS**:NSView 开 layer-backed + CAMetalLayer(挂在专用底层子视图里);`drawableSize` 以 raster 请求的尺寸为准,主线程只同步 frame 与 contentsScale;拖拽缩放时等一帧新尺寸的内容与窗口尺寸同一事务提交;快捷键由框架按 KeyData 处理(Cmd 按住时 AppKit 吞掉的 keyUp 用本地监视器补发);Space/全屏切换时 metrics 更新。
