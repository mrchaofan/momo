# 新维护者入门

这份文档给**刚接手 Momo 的维护者**：怎么把项目跑起来、读懂、改第一处代码，以及哪些规则一旦违反就会出现难查的崩溃。使用者视角见 [README.md](README.md)；每层怎么工作、为什么这样设计见 [ARCHITECTURE.md](ARCHITECTURE.md)。

1. [第一天：跑起来](#1-第一天跑起来)
2. [阅读路线](#2-阅读路线)
3. [代码地图：要改 X 去哪里](#3-代码地图要改-x-去哪里)
4. [必须遵守的规则](#4-必须遵守的规则)
5. [几种改动的完整步骤](#5-几种改动的完整步骤)
6. [调试技巧](#6-调试技巧)
7. [已知的坑](#7-已知的坑)
8. [文档维护约定](#8-文档维护约定)

---

## 1. 第一天：跑起来

按 README「快速开始」构建出 `out/debug/embedder/Momo.app`，然后跑一遍测试（各类测试的命令见 docs/09 §5）：

```bash
cd out/debug && ctest -j8
```

```bash
cd framework && npm test
```

- CTest 应当全部通过。没有 GPU、显示器或不允许绑定端口的环境里，Metal、CVDisplayLink、inspector 网络用例会显示 `Skipped`，这不是失败。
- `npm test` 输出四段 `0 failed`（单元测试、counter / todo / 浏览器示例端到端）。

然后亲眼看一次应用：直接运行 `Momo --dev`（默认的 `hello_frame` 动画），再构建并运行 `examples/counter`，在终端里看日志。

## 2. 阅读路线

不要从头逐文件读。按"一帧"和"一次点击"两条主线读，对照 ARCHITECTURE.md 里的图。几乎每个头文件开头都有一段注释，写明了**线程约定**和**生命周期**：先读那段注释，再读代码。

**半天：建立全局图**

1. ARCHITECTURE.md §1 总览 + §6 端到端走查。
2. `embedder/macos/main_mac.mm` → `embedder/common/application.cc` 的 `Create` / `Launch` / `CreateEngine` / `Quit` / `Shutdown` / `TearDown`。这就是整个进程的生命线。
3. `examples/hello_frame/main.js`：不经过框架，直接看 JS 怎么调用引擎。

**一天：跟一帧**

```
engine/shell/animator.cc        RequestFrame → BeginFrame → Render
engine/shell/vsync_waiter.cc    PlatformVsyncWaiter
embedder/macos/mac_vsync_displaylink.mm
engine/shell/engine.cc          OnAnimatorBeginFrame
engine/ui/platform_dispatcher_binding.cc     UiBindings::BeginFrame、viewport.render
engine/ui/scene_bindings.cc     SceneBuilder → LayerTree
engine/shell/shell.cc           Create 里的 on_draw → PostToRasterizer
engine/shell/rasterizer.cc      Draw → DrawToSurface
engine/graphics/render_backend.cc、gpu/metal_rasterizer.mm
embedder/macos/mac_surface_metal.mm   AcquireFrame / present
```

**一天：跟一次点击**

```
embedder/macos/mac_window.mm             MacWindow::SendEvent → SendToMomo
embedder/macos/mac_mouse_events.mm       MacMouseInput::Dispatch(翻译)
embedder/common/pointer_dispatcher.cc    Dispatch(决定接收方)
embedder/common/window_controller.cc     OnPointerPacket
engine/shell/shell.cc                    OnPointerPacket → PostToEngine
engine/ui/platform_dispatcher_binding.cc UiBindings::OnPointerPacket
framework/.../gestures/binding.ts        handlePointerPacket
framework/.../views/binding.ts           drawFrame
framework/.../rendering/object.ts        PipelineOwner、PaintingContext
```

**之后按需**：base 的对象模型（`base/object.h`、`base/event_loop.h`）、V8 绑定（`engine/runtime/class_builder.h`）、扩展（docs/07、`engine/native_host/native_host.h`）。

## 3. 代码地图：要改 X 去哪里

| 要做的事 | 主要文件（按顺序改） | 别忘了同步 |
|---|---|---|
| 新增/修改 `momo:ui` API | `engine/ui/*_bindings.cc`（用 `ClassBuilder`） | `framework/packages/momo/src/ui/momo-ui.d.ts`（JS⇄C++ 契约）、`framework/test/mock/momo_ui.ts`（Node mock）。步骤见 §5.1 |
| 新增平台服务（对话框、通知…） | `embedder/common/platform_services.h` 加方法 → `mac_platform_integration.mm`（`MacServices`）实现 → `platform_channels.cc` 加一个 `HostChannel` 并 `Register` | 框架 `services/` 加基于 `MethodChannel` 的封装；`win_services.cc`；`testing/mock_platform.h`；`services_test.ts`。**不改 engine** |
| 新增窗口操作（全屏、透明…） | `platform_window.h` 加方法（平台可以不支持的默认返回 `Unsupported(...)`）→ `mac_window.mm` 实现 → `window_channel.cc` 里 `RegisterWindowMethod` 一行 | 框架 `services/widget.ts` 的 `Widget` 加方法（宿主推送的变化加事件），需要声明式时加到 `WindowChrome`；mock 宿主（`momo_ui.ts` 的 `handleWindowCall`）；`window_channel_unittests.cc`；`host_contract_test.ts`。**不改 engine** |
| 新增应用操作（Dock 角标…） | `platform_app.h` → `mac_platform_integration.mm` → `app_channel.cc` 里 `Register` 一行 | 框架 `services/app.ts`；mock 宿主的 `momo/app`。**不改 engine** |
| 新增菜单 role | `MenuRole`（`menu_model.h`，含名字表）→ `mac_menu.mm` 的 `kRoles` 加一行；Windows 在 `win_menu.cc` 的 `kRoles` 加一行并在 `Application::PerformMenuRole` 里执行 | 框架 `MenuItemRole` 加名字 |
| 应用生命周期、启动与退出 | `embedder/common/application.cc`、`window_manager.cc` | `application_unittests.cc`（退出流程与关闭顺序有专门断言） |
| 窗口/输入行为 | `embedder/macos/mac_window.mm`（`MomoContentView`）、`pointer_dispatcher.cc` | 输入类型在 `engine/platform/input_types.h`，按键编码在 `embedder/common/key_codes.h`；JS 侧解码在 `gestures/converter.ts` |
| 帧调度、背压 | `engine/shell/animator.cc`、`base/pipeline.h` | `animator_unittests.cc` |
| 渲染（层、DisplayList、Skia）、GPU 后端 | `engine/graphics/`（后端在 `render_backend.cc`、`gpu/`） | golden 基准图 `engine/graphics/testdata/`；回退链在 `Rasterizer::SelectBackend` |
| 文本排版 | `engine/text/`、`engine/ui/text_bindings.cc` | 框架 `painting/text_painter.ts` |
| 消息通道/编解码 | `engine/common/message_channel_hub.*`、`standard_codec.*` | JS 侧 `services/standard_codec.ts` 必须保持同一线格式 |
| 命令行参数 | `embedder/common/app_config.cc` | README 的参数表 |
| View / 布局 / 手势 / Material | `framework/packages/momo/src/{views,rendering,gestures,material}/` | `framework/test/*_test.ts`。步骤见 §5.2 |
| 扩展的宿主能力表（C ABI） | `extensions/sdk/include/momo/momo_host.h`、`engine/native_host/native_host.cc` | **只能在结构体末尾追加字段**；JS 与原生之间的事都用 Node-API，不进这张表；docs/07 §3 |
| 扩展 SDK 打包 | `extensions/CMakeLists.txt`、`extensions/sdk/lib/cmake/MomoExtSdk/` | CTest `ExtensionSdk.StandaloneCxxBuild` |
| 运行时自带的 JS 模块怎么构建、怎么解析 | `embedder/CMakeLists.txt`（`momo_builtin`）、`framework/scripts/build_builtin.mjs`；解析在 `engine/runtime/node_bootstrap.cc` | `tools/package_release.py`；docs/09 §4.1 |
| 内置扩展 webview | `extensions/webview/`（`js/index.ts` + `native/`） | `framework/test/mock/ext_webview.ts`（Node 测试用的替身）、`extensions/webview/README.md` |
| 打包工具 | `framework/packages/cli/momo.mjs` | docs/09 §4.3 |

## 4. 必须遵守的规则

下面这些规则不是风格偏好（风格见 docs/10）。违反它们通常不会马上报错，而是在关闭时、resize 时或偶发地崩溃。

### 4.1 线程

- **每个对象只在它的归属线程上使用和析构。** 继承 `momo::Object` 的类在方法开头写 `MOMO_DCHECK_ON_AFFINITY_THREAD();`，Debug 构建会断言。
- **V8 只在 ui 线程，GPU 只在 raster 线程，平台 API 只在 platform（主）线程。** 跨线程只能投递任务，数据要么复制，要么是不可变的。
- **帧路径上不允许阻塞等待。** `PostTaskAndWait` 只用于关闭与视图解绑的汇合点。
- **回调一律在调用方线程执行。** 谁注册回调，回调就在谁的线程上被调用；例外只有注册时用 runner 参数显式指定了执行线程。系统线程（CVDisplayLink、DwmFlush、inspector）和扩展线程只负责投递，从不直接调用别人的回调。写新接口时，要么回调回到注册线程，要么让调用方显式传 runner。

### 4.2 所有权与生命周期

- 默认 `std::unique_ptr`。有线程归属的对象用 `ObjectPtr<T>`（释放 = `DeleteLater`，在归属线程的安全点析构）。
- **不要用 `shared_ptr` 表达跨线程生命周期。** 别的线程只拿 `WeakPtr<T>`，并通过 `PostToObject(weak, fn)` 回到归属线程判活后调用。
- 退出与销毁是分开的两步（同 Electron）：退出流程（`Quit` / `Exit`）只关闭窗口并让消息循环结束，不销毁任何东西；销毁（`Application::TearDown`）在循环结束之后逐级进行。没有"直接终止"的入口：不强行终止 JS，也不强杀进程。调整前先读 ARCHITECTURE §3.8，并跑 `ApplicationTest`。

### 4.3 依赖方向

- engine **不得** include `embedder/` 下的任何文件。engine 需要平台能力时，在 `engine/platform/` 声明纯接口，由 embedder 实现。
- **engine 只认识视图**（画面、尺寸、输入、可见性）。窗口的外观与行为、菜单、剪贴板、输入法属于宿主，经宿主通道（ARCHITECTURE §3.10）。不要为这类需求往 `Shell::Delegate`、`WindowClient` 加方法；窗口的系统焦点也不经引擎。
- `v8.h` 只能出现在 `engine/runtime/`、`engine/ui/` 的**实现文件**里；Skia 头只能出现在 `engine/graphics/`、`text/`、`assets/` 的实现文件里。
- 平台实现库 `momo_embedder_macos` 只链接 `momo_engine_platform`（接口）和系统 SDK。

### 4.4 契约文件：两边必须一起改

- `framework/packages/momo/src/ui/momo-ui.d.ts` 是 JS⇄C++ 的接口契约。改绑定必须同步改它，以及 Node mock（`framework/test/mock/`）。
- 宿主通道的协议（ARCHITECTURE §3.10 的表）有两份实现：`embedder/common/*_channel.cc` 与 mock 宿主 `framework/test/mock/momo_ui.ts`。`host_contract_test.ts` 在两边都跑，把它们绑在一起。
- `StandardCodec` 的线格式在 C++（`engine/common/standard_codec.cc`）和 JS（`services/standard_codec.ts`）各实现一份，必须一致。
- 扩展的宿主能力表（`momo_host.h`）：`MomoHost` **只增不改不删，只能在末尾追加**。

### 4.5 Windows 代码

`embedder/windows/` 与 macOS 同构（ARCHITECTURE §3.13），但**从未在 Windows 上运行过**。

- 纯逻辑（鼠标消息翻译 + 派发器、多 DPI 屏幕坐标、窗口框架命中、键码表）在 `momo_embedder_windows_portable_unittests` 里，随 `ctest` 在 macOS 上运行。新加的 Windows 逻辑尽量写成不依赖 `<windows.h>` 的纯 C++ 放进这个库。
- 修改 `embedder/common/` 的接口（`PlatformWindow`、`WindowDelegate`、`PlatformIntegration`）或 `engine/platform/` 契约时，要同步 Windows 实现，并在改动说明里写明"未在 Windows 上运行验证"。

## 5. 几种改动的完整步骤

### 5.1 给 `momo:ui` 加一个方法（以 `Canvas.drawArc` 为例）

1. `engine/graphics/display_list.h`：如果是新的绘制指令，加 `OpType` 和 `DisplayListBuilder` 方法；在 `graphics/skia/` 的重放代码里处理它。
2. `engine/ui/painting_bindings.cc`：在 `JsCanvas` 的 `ClassBuilder` 上 `.Method("drawArc", &DrawArc)`，用 `js_args.h` 解码参数。
3. `momo-ui.d.ts` 加类型声明；`framework/test/mock/momo_ui.ts` 让 mock 记录这个调用。
4. 测试：`engine/graphics/graphics_unittests.cc`（C++ 层）和/或 `engine/ui/ui_unittests.cc`（经 JS 调用）；需要像素比对时加 golden。

### 5.2 新增一个 View（组件）

1. 在 `framework/packages/momo/src/views/`（或 `material/`）实现；需要新布局/绘制时，在 `rendering/` 写 RenderObject。从对应目录的 `index.ts` 导出。
2. 在 `framework/test/` 用 `testViews` + `ViewTester` 写测试。依赖字形宽度的断言加 `{ ahemText: true }`；只在 mock 下才能观察到的断言加 `{ mockOnly: true }`。
3. `npm test`（Node）+ `ctest -R FrameworkTests`（真实引擎）都要过。

### 5.3 改 embedder 的 macOS 行为

1. 改 `embedder/macos/*.mm`。所有 AppKit 调用都在主线程；raster 线程只会调用 surface 的 `AcquireFrame` / `present` / `FallBackToSoftware`。两个平台共有的逻辑在 `embedder/common`（`FallbackSurface`、`PresentQueue`、`PointerDispatcher`、`key_codes.h`），不要在平台层各写一份；GPU 设备是应用级的（`MacMetalDevice`），不要在 surface 里各建一份。
2. `momo_embedder_macos_unittests` 里需要显示器的用例在沙箱里会跳过，所以**这类改动必须在真机上手动验证一次**。

## 6. 调试技巧

- **日志**：`MOMO_LOG(INFO|WARNING|ERROR|VERBOSE) << ...`，输出到 stderr。从终端启动 `Momo.app/Contents/MacOS/Momo` 就能看到。
- **线程断言**：`MOMO_DCHECK_ON_AFFINITY_THREAD()` 失败会打印"Called off the object's affinity thread"。看到它，就说明某处跨线程直接调用了对象。
- **JS**：`--dev` 启动后用 Chrome DevTools 连 `localhost:9229`；`--inspect-brk` 在执行入口前停下。
- **渲染问题**：`MOMO_SOFTWARE_RENDERING=1` 切到 CPU 光栅。如果问题消失，就是 Metal 路径的问题。
- **不开窗口复现**：`momo_headless`（docs/09 §5）。
- **ThreadSanitizer**：`cmake -B out/tsan -DMOMO_ENABLE_TSAN=ON`。sanitizer 构建里没有 V8，JS 相关路径用占位驱动代替。

## 7. 已知的坑

- **`nextDrawable` 可能返回 nil**：此时直接丢帧，不能阻塞等待。
- **AppKit 的键盘怪癖**：按住 Command 时 `-[NSApplication sendEvent:]` 会吞掉 `keyUp`，`MomoContentView` 用本地事件监视器补发并按事件对象去重。`flagsChanged` 事件不能访问 `characters`（会抛 ObjC 异常），修饰键只看 `modifierFlags`。
- **在 GCD 主队列的任务里进入嵌套 run loop，platform EventLoop 就不再被服务**。所以上下文菜单经 `CFRunLoopPerformBlock` 弹出（ARCHITECTURE §3.11）；新加会进入系统模态循环的功能时同样处理。
- **JS 运行时先于引擎的其余部分销毁**，而 io 线程上的解码、通道回复可能在这之后才回来。所以绑定层中所有异步回调都要用 `WeakPtr<Impl>` 判活（见 `engine/ui/ui_impl.h`）。
- **入口不等待顶层 await**：应用入口要同步调用 `runApp`。
- **框架代码不直接调 `setTimeout`**：延迟任务用 `SchedulerBinding.instance.scheduleDelayed`，测试里才能换成假时钟。
- **沙箱里跳过的测试不代表这些路径没问题**：Metal、CVDisplayLink、inspector 网络用例只在真机上运行。

## 8. 文档维护约定

每类信息只在一个地方维护，其他地方用链接引用：

| 信息 | 唯一来源 |
|---|---|
| 怎么用（构建运行、写应用、API 速查、命令行） | README.md |
| 代码怎么工作、为什么这样设计（含宿主通道协议表） | ARCHITECTURE.md |
| 原生扩展 | docs/07 |
| 构建、依赖、打包、测试命令、发布 | docs/09 |
| 编码规范 | docs/10 |
| 现状、未验证、已知限制、没做的功能 | docs/11 |
| `momo:webview` 的接口 | `extensions/webview/README.md` |
| 某个类的线程约定与生命周期 | 该类头文件开头的注释 |
| 维护规则、代码地图、改动步骤 | 本文 |

- 改了代码里的类名、文件名或流程，要同步改 ARCHITECTURE.md；源码注释里以 `ARCHITECTURE.md §x.y`、`docs/NN §x` 引用文档，小节号变了要一起改（`git grep -n '§'`）。
- 文档里写的类名必须在代码中真实存在；没有验证过的行为写"未验证"；不写历史流水账（"某某已删除"）。
- 不为目录另写 README：目录是什么由 ARCHITECTURE.md 的模块表说明。
- ARCHITECTURE.md 的图是 `docs/images/*.svg`，手写的 SVG，直接编辑文本即可。
