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
│   ├── platform_window.h       # PlatformWindow / WindowDelegate
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
5. AssetResolver 解析 bundle → Shell::Create() → 注册通道、绑定窗口 → Shell::Run()(Application::LaunchEngine;Launch 到此返回,
   状态仍是 Launching, 主线程进入事件循环)
   a. engine::Shell::Create(TaskRunners, Settings, on_exit_requested): 只组装引擎(不创建 JS 运行时、不等待其他线程;
      原生扩展由 JS 加载, 这里不加载任何东西, 见 docs/07);主显示器刷新率作为 vsync 回退定时器频率
   b. 注册宿主通道(AppChannel: momo/app;WindowChannel: momo/window;PlatformChannels: momo/clipboard、momo/url、momo/textinput)
   c. WindowManager::AttachEngine: platform.CreateVsyncSource()(应用只有一个;不支持时为空 → 定时器回退)经 Shell::SetVsyncSource 移交 Shell,
      再逐个窗口 Shell::AttachView;帧时钟跟随 id 最小的窗口所在的显示器(platform.SetVsyncWindow)
   d. Shell::Run(异步)。ui 线程: 创建 JsRuntime(V8 Isolate/Context) → 安装 momo:* 绑定 → 加载并执行 JS bundle → 框架调用 runApp()
   e. 结果回到 platform 线程(Application::OnRuntimeStarted): 成功 → Running;bundle 加载/执行失败 → 有序退出, 退出码 1
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

**应用生命周期与窗口无关**:Suspended 只表示应用被隐藏(Mac `applicationDidHide`,经 `OnAppVisibilityChanged`);窗口的最小化/隐藏只影响该窗口的视图是否出帧(`WindowDelegate::OnWindowStateChanged` → `Shell::OnViewVisibilityChanged`)。没有窗口也是合法状态。状态变化经 `Shell::SetAppSuspended` 告诉引擎是否出帧, 经 `momo/app stateChanged` 告诉 JS。前台状态(`OnAppActiveChanged`)与 Dock 重新激活(`OnReopen`)经 `momo/app activeChanged` / `activate`。

退出应用有两个入口(同 Electron), 没有"直接终止":`Quit()`(Cmd+Q、菜单退出、JS `app.quit()`)依次询问 `beforeQuit` → 每个窗口的 `closeRequested`(允许的关闭)→ `willQuit`, 任一步被 JS 取消即中止, 全部允许后 `Shutdown`;启动期间(bundle 还在执行)请求退出走同一条流程。`Exit(code)`(JS `app.exit(code)`、`process.exit(code)`、启动失败)不询问, 关闭全部窗口后 `Shutdown`。`Shutdown` 只做两件事: 进入 Terminating(JS 收到这个状态)、请求消息循环结束——不销毁任何东西。销毁(`TearDown`, §4)在消息循环结束之后、`Run()` 返回之前进行。重复请求合并。不强行终止 JS, 也没有超时强杀: JS 不返回时退出就等着它。可选 `Application::Observer` 观察状态变化与关闭步骤(日志/测试)。

窗口(docs/02 §2 多视图):**所有窗口对等**, 注册表归 `WindowManager`(`window_manager.h`), 它是 `WindowChannel::Host`:按 JS 的 `momo/window create` 分配视图 id、建窗口并绑定到同一个 Shell;`close` 时先解绑再在安全点销毁窗口。任一窗口的关闭按钮都先经 `momo/window closeRequested` 询问 JS, 允许后只关闭该窗口;**最后一个窗口关闭后应用不退出**, `Application` 只发 `momo/app windowAllClosed`(同 Electron 的 window-all-closed 事件, 但没有"无监听器即退出"的隐式默认)。启动窗口(视图 0)只是 bundle 执行前就存在的那个窗口, 同样可以被关闭;`--no-startup-window` 时不创建。

### 引擎装配(Application::LaunchEngine)

Application 直接持有一个 `engine::Shell`(所有窗口共用)。启动时把 embedder 侧对象翻译成引擎依赖:AssetResolver 结果(bundle、资源目录, 以及入口给出的缓存目录下的 V8 code cache 目录) + `--dev` + 主显示器刷新率 → `engine::Settings`;`Shell::Create(task_runners, settings, on_exit_requested)`(只组装引擎;on_exit_requested 接 `process.exit`)→ 在 `shell.hub()` 上注册宿主通道 → `WindowManager::AttachEngine` 创建应用唯一的 VsyncSource(`CreateVsyncSource`;不支持则为空, Shell 回退定时器)交给 `Shell::SetVsyncSource`, 再为每个窗口 `WindowController::AttachEngine` → `Shell::Run(done)` 异步启动 JS 运行时, 结果回来后才进入 Running。销毁时先 `Shell::Shutdown`(顺序见 §4), 再 `WindowManager::DetachEngine`、移除宿主通道。

### WindowController(common)

把 `PlatformWindow` 的视图相关回调(尺寸/DPI 变化、输入事件、焦点)转发给 `engine::Shell` 对应的视图;纯通知类的窗口事件(状态、焦点、屏幕框、窗口控制按钮占用区域、首帧)直接经注入的 `WindowChannel` 通知 JS, 需要 WindowManager 做决定的(关闭请求、焦点归属、IME 路由)经 `WindowController::Client` 接口交给它;平台窗口每次上报当前的原始值, 去重(与上次相同则不转发)只在这里做;反向只承接引擎对视图的请求(指针捕获转交、平台视图挂载/聚焦)。窗口外观与行为(标题、光标、位置、标题栏…)的请求不经过这里: `momo/window` 通道直接调用 `PlatformWindow`。输入事件在 platform 线程收到后打包为 `PointerDataPacket`/`KeyData` 投递至 ui 线程。引擎附着前收到的 metrics/焦点会被缓存,附着时补发。原生关闭请求**总是先拦截**(`OnCloseRequested` 返回 false),交给 `WindowManager::RequestCloseWindow` 异步询问 JS。

## 4. 生命周期与关闭顺序

退出与销毁分两步(同 Electron), 销毁必须严格有序, 否则会在线程销毁上踩崩溃:

```
Quit(JS 可取消) / Exit(code): 窗口在流程里关闭(JS 还在, 收到各自的 closed)
 → Application::Shutdown: 进入 Terminating(通知 JS)
 → 0) platform EventLoop::Terminate: 当前任务返回后排空、析构 DeleteLater 对象, 再退出原生循环
 —— 消息循环结束, Run() 返回之前: Application::TearDown, 逐级销毁 ——
 → 1) platform 线程: 关闭原生扩展的宿主能力表;全部平台视图移出窗口、交给扩展释放(此后引擎不再调用扩展, docs/07 §4)
      ui 线程: 销毁 JsRuntime(Isolate Dispose;扩展登记的清理函数在这里执行), 再停止帧调度
 → 2) raster 线程: 释放 LayerTree 与 GPU 资源,销毁 GrDirectContext(同步停用各窗口的 surface)
 → 3) 销毁还开着的 RenderSurface 与 PlatformWindow(platform 线程;正常退出时窗口已在流程里关闭)
 → 4) ThreadHost::Join(): ui → raster → io(各自排空已到期任务、清理 DeleteLater 对象)
```

第 1 步销毁 JsRuntime 时要与 ui 线程汇合: 此前投递给 JS 的事件都先执行完;JS 不返回(`--inspect-brk` 等待调试器连接、脚本死循环)时
退出停在这里等它。不强行终止 JS 的执行, 也不强杀进程。

## 5. 平台入口差异

- **Windows**:`wWinMain`;`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 必须在任何窗口创建前调用;消息循环即 platform runner,利用 `PostMessage` 到隐藏消息窗口实现跨线程投递任务。`WinAppDelegate`(对应 `MacAppDelegate`)经 `CreateWinPlatformIntegration(&delegate)` 接收 `WM_ACTIVATEAPP`(→ `OnAppActiveChanged`);单实例(按 exe 名)的后续启动转发给首个实例 = 重新激活(激活最前的可见窗口 + `OnReopen`, 同点击 Dock 图标);`WM_ENDSESSION` 时 `Exit(0)`(结束会话不等待应用, 无法询问 JS;销毁在消息循环结束后进行, 系统可能在那之前就结束进程)。所有窗口关闭后不退出, 由 JS 决定。
- **macOS**:`main_mac.mm` 创建平台集成(内部 `[NSApplication sharedApplication]`)并设置 `MacAppDelegate`(含最小主菜单:隐藏/退出);platform runner 基于 GCD 主队列(由主 CFRunLoop 在 common modes 下服务,resize 拖拽期间同样执行),退出循环用 `[NSApp stop:]` + 唤醒事件;`applicationShouldTerminate` 返回 `NSTerminateCancel` 并转 `Quit`(JS 可取消), `applicationShouldTerminateAfterLastWindowClosed` 返回 NO,由我们自己有序关闭后让 `main` 正常返回。

### 命令行(`AppConfig::Parse`)

`--bundle=<path>`、`--assets=<dir>`、`--title=<text>`、`--width=<px>`、`--height=<px>`、`--dev`。未知参数与系统参数(`-psn_*`、`-NS*`)忽略。资源根目录由入口提供:Mac `NSBundle.mainBundle.resourcePath`,Win exe 所在目录;默认布局 `<resources>/{bundle.js, native/, assets/}`(docs/09 §4)。应用的缓存目录(V8 code cache 在其 `v8/` 下)同样由入口提供, 每个应用一个:Mac `~/Library/Caches/<bundle identifier>`,Win `%LOCALAPPDATA%\<exe 名>`(与单实例的 app id 相同)。

## 6. 测试策略

`embedder/common` 不包含任何平台 API,可用 mock 的 `PlatformIntegration` 做单元测试(启动顺序、关闭顺序、生命周期状态机)。双端入口文件保持 <100 行,仅做翻译。
