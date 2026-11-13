# 01 · Embedder 层设计

Embedder 是**可执行程序本身**:它拥有进程入口,运行原生消息循环,实现 engine 声明的平台契约(`engine/platform/`,见 docs/03),装配 Engine,并在合适的时机启动 JS 环境。它是唯一认识平台具体实现的层;engine 不依赖它。

## 1. 目录结构

```
embedder/
├── CMakeLists.txt
├── common/                     # 平台无关的装配与生命周期逻辑
│   ├── application.h/.cc       # Application: 进程级单例,生命周期状态机
│   ├── window_controller.h/.cc # 窗口 ↔ 引擎视图的粘合(事件转发、尺寸同步)
│   ├── app_config.h/.cc        # 启动配置(bundle 路径、窗口参数、命令行解析)
│   ├── asset_resolver.h/.cc    # 定位 JS bundle 与资源(app 包内 / 开发模式本地目录)
│   ├── platform_integration.h  # embedder 的平台抽象: 工厂 + 原生循环(docs/03)
│   ├── platform_window.h       # PlatformWindow / WindowDelegate / WindowSpec
│   ├── display.h               # DisplayInfo
│   ├── engine_platform.h       # 命名空间别名 platform:: = momo::engine::platform
│   └── testing/mock_platform.h # 单测用 mock PlatformIntegration(无 OS API)
├── windows/
│   ├── main_win.cc             # wWinMain 入口
│   ├── win_app_delegate.h/.cc  # Win32 消息循环接入、单实例、DPI 感知声明
│   └── win_*.h/.cc             # 平台实现: 窗口、surface、vsync、IME、服务、原生循环(docs/03)
├── macos/
│   ├── main_mac.mm             # main 入口,NSApplication 启动
│   ├── mac_app_delegate.h/.mm  # NSApplicationDelegate,生命周期事件转发
│   └── mac_*.h/.mm             # 平台实现: 窗口、Metal/软件 surface、CVDisplayLink、IME、原生循环(docs/03)
└── headless/
    └── headless_main.cc        # momo_headless: 无窗口宿主(离屏软件 surface, 测试运行器)
```

## 2. 启动时序(两端一致的逻辑顺序)

```
1. 进程入口(wWinMain / main)
2. 解析 AppConfig(bundle 路径、窗口初始参数、--dev 调试开关)
3. Application::Create(options, platform)
   a. 平台集成由入口创建并注入(mac: CreateMacPlatformIntegration();win: CreateWinPlatformIntegration())
   b. 创建 OS 主线程的 `EventLoop`(`base::NativeMessagePump` 包装 `platform.native_loop()`)并绑定当前线程(Application 自身是归属它的 `momo::Object`);ui/raster/io 由 Application 持有的 `engine::ThreadHost` 创建;四者组成 TaskRunners
4. Application::Launch(): 创建启动窗口(隐式视图 0)的 PlatformWindow + RenderSurface 并 Show(此时只有原生空窗口,显示闪屏底色)
5. AssetResolver 解析 bundle → Shell::Create() → WindowController::AttachEngine()(Application::LaunchEngine)
   a. engine::Shell::Create(TaskRunners, Settings, on_created);主显示器刷新率作为 vsync 回退定时器频率。on_created 在 JS bundle
      执行前回调: 注册宿主通道(WindowChannel: momo/window、momo/app;PlatformChannels: momo/clipboard、momo/url、momo/textinput)
   a'. AttachWindow: platform.CreateVsyncSource(window)(不支持时为空 → 定时器回退)随 Shell::AttachView 移交 Shell
   b. ui 线程: 创建 JsRuntime(V8 Isolate/Context) → 安装 momo:* 绑定
   c. ExtensionHost 扫描并加载声明的原生扩展(见 docs/07)
   d. 加载并执行 JS bundle → 框架调用 runApp()
6. 首帧: JS 提交第一个 Scene → raster 线程绘制 → present → 替换闪屏
7. Application::Run(): 运行 platform EventLoop(其 pump 即原生消息循环:Win32 GetMessage 泵 / NSApplication run),Terminate 后在安全点清理 DeleteLater 对象再返回
```

步骤 4–5 任一失败:`Launch()` 返回错误前已按 §4 完成有序清理(状态 Terminating,退出码 1)。bundle 缺失在 `--dev` 下降级为警告,以空引擎启动。

"合适时机创建 JS 环境"的含义:窗口与 GPU 表面的创建(步骤 4)和 V8 初始化(5.b)并行化——V8 snapshot 反序列化与 bundle 读取在 ui/io 线程进行,不阻塞首个原生窗口出现。

## 3. 关键类

### Application(common)

进程级生命周期状态机(每进程由入口创建一个,不做全局单例,避免全局可变状态):

```
NotRunning → Launching → Running ⇄ Suspended → Terminating
   │            └────(启动失败)─────────────────────┘
   └──(未 Launch 即销毁)───────────────────────────┘
```

Suspended = 应用被隐藏(Mac `applicationDidHide`,经 `OnAppVisibilityChanged`)**或**所有窗口都最小化(`WindowDelegate::OnVisibilityChanged`);任一窗口可见且应用未隐藏才回到 Running。进入 Running/Suspended/Terminating 时经 `Shell::NotifyLifecycleEvent` 分发 `resumed/suspended/terminating` 给 JS(M2 起走 `momo/lifecycle` 通道)。

退出应用:`RequestTerminate()`(Cmd+Q、系统注销)先经 `momo/app requestExit` 询问 JS(可取消,如"未保存提示"),放行后 `Terminate()`;重复请求合并;JS 长时间不回复(ui 线程卡死)时强制退出。`Terminate()` 幂等,直接执行 §4。可选 `Application::Observer` 观察状态变化与关闭步骤(日志/测试)。

窗口(docs/02 §2 多视图):**所有窗口对等**。Application 持有全部窗口(视图 id → `WindowController`), 是 `WindowChannel::Host`:按 JS 的 `momo/window create` 分配视图 id、建窗口并绑定到同一个 Shell;`close` 时先解绑再在安全点销毁窗口。任一窗口的关闭按钮都先经 `momo/window closeRequested` 询问 JS, 允许后只关闭该窗口;**最后一个窗口关闭后退出应用**(同 Electron 的 window-all-closed 默认行为)。启动窗口(视图 0)只是 bundle 执行前就存在的那个窗口, 同样可以被关闭。

### 引擎装配(Application::LaunchEngine)

Application 直接持有一个 `engine::Shell`(所有窗口共用)。启动时把 embedder 侧对象翻译成引擎依赖:AssetResolver 结果 + `--dev` + 主显示器刷新率 → `engine::Settings`;`Shell::Create(task_runners, settings, on_created)`(on_created 里注册宿主通道);为启动窗口 `CreateVsyncSource`(不支持则为空, Shell 回退定时器)后 `WindowController::AttachEngine`。关闭时先移除宿主通道, 再 `DetachEngine`(raster 停用 surface), 再 `Shell::Shutdown`。

### WindowController(common)

把 `PlatformWindow` 的视图相关回调(尺寸/DPI 变化、输入事件、焦点)转发给 `engine::Shell` 对应的视图;窗口级事件(关闭请求、最小化、IME、屏幕框、窗口控制按钮占用区域)交给 Application, 由它经宿主通道通知 JS;反向只承接引擎对视图的请求(指针捕获转交、平台视图挂载/聚焦、原生句柄)。窗口外观与行为(标题、光标、位置、标题栏…)的请求不经过这里: `momo/window` 通道直接调用 `PlatformWindow`。输入事件在 platform 线程收到后打包为 `PointerDataPacket`/`KeyData` 投递至 ui 线程。引擎附着前收到的 metrics/焦点会被缓存,附着时补发。原生关闭请求**总是先拦截**(`OnCloseRequested` 返回 false),交给 `Application::RequestCloseWindow` 异步询问 JS。

## 4. 生命周期与关闭顺序

关闭必须严格有序,否则会在线程销毁上踩崩溃:

```
最后一个窗口关闭 / 用户退出应用(JS 可取消) → Application::Terminate
 → 0) WindowController 解绑: raster 线程同步停止使用 surface
 → 1) ui 线程: 停止帧调度,通知扩展 unload,销毁 JsRuntime(Isolate Dispose)
 → 2) raster 线程: 释放 LayerTree 与 GPU 资源,销毁 GrDirectContext
 → 3) 销毁 RenderSurface 与 PlatformWindow(platform 线程)
 → 4) ThreadHost::Join(): ui → raster → io(各自排空已到期任务、清理 DeleteLater 对象)
 → 5) platform EventLoop::Terminate: 当前任务返回后排空、析构 DeleteLater 对象(如 VsyncSource),再退出原生循环
```

## 5. 平台入口差异

- **Windows**:`wWinMain`;`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 必须在任何窗口创建前调用;消息循环即 platform runner,利用 `PostMessage` 到隐藏消息窗口实现跨线程投递任务。
- **macOS**:`main_mac.mm` 创建平台集成(内部 `[NSApplication sharedApplication]`)并设置 `MacAppDelegate`(含最小主菜单:隐藏/退出);platform runner 基于 GCD 主队列(由主 CFRunLoop 在 common modes 下服务,resize 拖拽期间同样执行),退出循环用 `[NSApp stop:]` + 唤醒事件;`applicationShouldTerminate` 返回 `NSTerminateCancel` 并转 `RequestTerminate`(JS 可拦截),由我们自己有序关闭后让 `main` 正常返回。

### 命令行(`AppConfig::Parse`)

`--bundle=<path>`、`--assets=<dir>`、`--title=<text>`、`--width=<px>`、`--height=<px>`、`--dev`。未知参数与系统参数(`-psn_*`、`-NS*`)忽略。资源根目录由入口提供:Mac `NSBundle.mainBundle.resourcePath`,Win exe 所在目录;默认布局 `<resources>/{bundle.js, assets/, extensions/}`(docs/09 §4)。

## 6. 测试策略

`embedder/common` 不包含任何平台 API,可用 mock 的 `PlatformIntegration` 做单元测试(启动顺序、关闭顺序、生命周期状态机)。双端入口文件保持 <100 行,仅做翻译。
