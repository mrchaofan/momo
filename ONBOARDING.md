# 新维护者入门

这份文档给**刚接手 Momo 的维护者**：怎么在一天内把项目跑起来、读懂、改第一处代码，以及哪些规则一旦违反就会出现难查的崩溃。

- 使用者视角（怎么用框架写应用）见 [README.md](README.md)。
- 实现细节（每层怎么工作，含时序图与伪代码）见 [ARCHITECTURE.md](ARCHITECTURE.md)。
- 设计取舍与里程碑见 `docs/`（从 [docs/00-overview.md](docs/00-overview.md) 读起）。

## 目录

1. [第一天：跑起来](#1-第一天跑起来)
2. [阅读路线](#2-阅读路线)
3. [代码地图：要改 X 去哪里](#3-代码地图要改-x-去哪里)
4. [必须遵守的规则](#4-必须遵守的规则)
5. [常见改动的步骤](#5-常见改动的步骤)
6. [测试：什么时候跑什么](#6-测试什么时候跑什么)
7. [调试技巧](#7-调试技巧)
8. [已知的坑](#8-已知的坑)
9. [文档维护约定](#9-文档维护约定)
10. [技术债与下一步](#10-技术债与下一步)

---

## 1. 第一天：跑起来

按顺序执行，每一步都应该成功（环境要求见 README「快速开始」）：

```bash
python3 tools/fetch_deps.py
```

```bash
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

```bash
cmake --build out/debug
```

```bash
cd out/debug && ctest -j8
```

```bash
cd framework && npm install && npm test
```

预期结果：

- CTest 全部通过。在没有 GPU、显示器或不允许绑定端口的环境（CI 容器、沙箱）里，有 9 项会显示 `Skipped`（Inspector 网络用例、Metal、CVDisplayLink），这是正常的，不是失败。
- `npm test` 输出三行 `0 failed`（单元测试、counter 端到端、todo 端到端）。

然后亲眼看一次应用：

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev
```

会运行默认的 `hello_frame` 动画。再按 README 构建并运行 `examples/counter`，在终端里看日志。修改 bundle 后窗口会自动 hot restart。

## 2. 阅读路线

不要从头逐文件读。建议按"一帧"和"一次点击"两条主线读，读的时候对照 ARCHITECTURE.md 里的图。

**半天：建立全局图**

1. [ARCHITECTURE.md](ARCHITECTURE.md) §1 总览 + §6 端到端走查。
2. `embedder/macos/main_mac.mm`（50 行）→ `embedder/common/application.cc` 的 `Create` / `Launch` / `LaunchEngine` / `Terminate`。这就是整个进程的生命线。
3. `examples/hello_frame/main.js`：不经过框架，直接看 JS 怎么调用引擎。

**一天：跟一帧**

按调用顺序读，每个文件只看相关函数：

```
engine/shell/animator.cc        RequestFrame → BeginFrame → Render
engine/shell/vsync_waiter.cc    PlatformVsyncWaiter::AwaitVsync
embedder/macos/mac_vsync_displaylink.mm
engine/shell/engine.cc          OnAnimatorBeginFrame
engine/ui/js_app_driver.cc      BeginFrame
engine/ui/platform_dispatcher_binding.cc     UiBindings::BeginFrame / DrawFrame、Render
engine/ui/scene_bindings.cc     SceneBuilder → LayerTree
engine/shell/shell.cc           OnEngineDraw
engine/shell/rasterizer.cc      Draw → DrawToSurface
engine/shell/render_backend.cc  选后端
embedder/macos/mac_surface_metal.mm   AcquireFrame / present
```

**一天：跟一次点击**

```
embedder/macos/mac_window.mm             mouseDown → dispatchPointer
embedder/common/window_controller.cc     OnPointerPacket
engine/shell/shell.cc                    OnPointerPacket → PostToEngine
engine/ui/platform_dispatcher_binding.cc              UiBindings::PointerPacket
framework/.../gestures/binding.ts        handlePointerPacket
framework/.../views/binding.ts           drawFrame
framework/.../rendering/object.ts        PipelineOwner、PaintingContext
```

**之后按需**：base 的对象模型（`base/object.h`、`base/event_loop.h` 的头部注释写得很完整）、V8 绑定（`engine/runtime/class_builder.h`）、扩展（`engine/extension/extension_host.h` + `extensions/sdk/include/momo/momo_extension.h`）。

> 读源码的小技巧：几乎每个头文件开头都有一段注释，写明了**线程约定**和**生命周期**。先读那段注释，再读代码。

## 3. 代码地图：要改 X 去哪里

| 要做的事 | 主要文件 | 别忘了同步 |
|---|---|---|
| 给 JS 新增/修改 `momo:ui` API | `engine/ui/*_bindings.cc`（用 `ClassBuilder`） | `framework/packages/momo/src/ui/momo-ui.d.ts`（JS⇄C++ 契约）、`framework/test/mock/momo_ui.ts`（Node mock） |
| 新增平台服务（对话框、菜单…） | `embedder/common/platform_services.h` → `embedder/macos/mac_platform_integration.mm`（`MacServices`）→ `embedder/common/platform_channels.cc` 注册通道 | 框架 `services/` 加 JS 封装；`embedder/windows/win_services.cc`；mock 平台。**不改 engine** |
| 新增窗口能力（全屏、透明、阴影…） | `embedder/common/platform_window.h`（可选能力 + `WindowCapability`）→ `embedder/macos/mac_window.mm` → `embedder/common/window_channel.cc` 里 `RegisterWindowMethod` 一行 | 框架 `services/widget.ts` 的 `Widget` 加方法/事件，需要声明式时加到 `WindowChrome`；Node mock 的宿主实现（`framework/test/mock/momo_ui.ts`）。**不改 engine** |
| 新增应用能力（Dock 角标、激活策略…） | `embedder/common/platform_app.h`（`PlatformApp` + `AppCapability`）→ `embedder/macos/mac_platform_integration.mm` → `embedder/common/app_channel.cc` 里 `Register` 一行 | 框架 `services/app.ts` 的 `app` 加方法；Node mock 的 `momo/app`。**不改 engine** |
| 应用生命周期、退出流程 | `embedder/common/application.cc`（`Quit` / `Exit` / `Terminate`）、`window_manager.cc` | `application_unittests.cc`（退出流程与关闭顺序有专门断言） |
| 窗口/输入行为 | `embedder/macos/mac_window.mm`（`MomoContentView`） | 输入类型在 `engine/platform/input_types.h`；JS 侧解码在 `gestures/converter.ts` |
| 帧调度、背压 | `engine/shell/animator.cc`、`base/pipeline.h` | `animator_unittests.cc` |
| 渲染（层、DisplayList、Skia） | `engine/graphics/` | golden 基准图 `engine/graphics/testdata/` |
| GPU 后端 | `engine/shell/render_backend.cc`、`engine/graphics/gpu/` | 回退链：`Rasterizer::CreateBackend` |
| 文本排版 | `engine/text/`、`engine/ui/text_bindings.cc` | 框架 `painting/text_painter.ts` |
| 消息通道/编解码 | `engine/common/message_channel_hub.*`、`standard_codec.*` | JS 侧 `services/standard_codec.ts` 必须保持同一线格式 |
| 启动、关闭、生命周期 | `embedder/common/application.cc` | `application_unittests.cc`（关闭顺序有专门断言） |
| 命令行参数 | `embedder/common/app_config.cc` | README 的参数表 |
| View / 布局 / 手势 | `framework/packages/momo/src/{views,rendering,gestures}/` | `framework/test/*_test.ts` |
| Material 控件 | `framework/packages/momo/src/material/` | `material_test.ts` |
| 扩展 C ABI | `extensions/sdk/include/momo/momo_extension.h`、`engine/extension/extension_host.cc`、`engine/ui/extension_bindings.cc` | **只能在结构体末尾追加字段**（见 §4）；新增能力时同步 C++ 封装 `momo_extension.hpp` |
| 扩展 SDK 打包 | `extensions/CMakeLists.txt`（`momo_ext_sdk_package`、install 规则）、`extensions/sdk/lib/cmake/MomoExtSdk/` | CTest `ExtensionSdk.StandaloneCxxBuild` |
| 打包工具 | `tools/momo-cli/momo.mjs` | README「打包发布」 |

## 4. 必须遵守的规则

下面这些规则不是风格偏好。违反它们通常不会马上报错，而是在关闭时、resize 时或偶发地崩溃。

### 4.1 线程

- **每个对象只在它的归属线程上使用和析构。** 继承 `momo::Object` 的类在方法开头写 `MOMO_DCHECK_ON_AFFINITY_THREAD();`，Debug 构建会断言。不确定当前在哪个线程时，看头文件里的"线程约定"注释。
- **V8 只在 ui 线程，GPU 只在 raster 线程，平台 API 只在 platform（主）线程。** 跨线程只能投递任务，数据要么复制，要么是不可变的（`shared_ptr<const DisplayList>`）。
- **帧路径上不允许阻塞等待。** `PostTaskAndWait` / `WaitableEvent` 只用于启动和关闭的汇合点。
- **回调一律在调用方线程执行。** 谁注册回调，回调就在谁的线程上被调用。例外只有一种：注册时用 runner 参数（`TaskRunner` / `MomoTaskRunner`）显式指定了执行线程。系统线程（CVDisplayLink、DwmFlush、inspector 网络线程）和扩展线程只负责投递（`PostToObject` / `runner->PostTask`），从不直接调用别人的回调。写新接口时，要么回调回到注册线程，要么让调用方显式传 runner。`ShellTest`、`TextureRegistryTest`、`ExtensionHostTest.CallbacksOnlyRunOnRegisteringThread` 都断言了这一点。
- engine / embedder 中不直接使用 `std::thread`、`std::mutex`、`condition_variable`；需要时先在 `base/` 提炼通用设施。

### 4.2 所有权与生命周期

- 默认 `std::unique_ptr`。有线程归属的对象用 `ObjectPtr<T>`（释放 = `DeleteLater`，在归属线程的安全点析构）。
- **不要用 `shared_ptr` 表达跨线程生命周期。** 别的线程只拿 `WeakPtr<T>`，并通过 `PostToObject(weak, fn)` 回到归属线程判活后调用。
- 已被 `ObjectPtr` 持有的对象，不要再手动调用 `DeleteLater()`。
- 关闭顺序（`Application::Terminate`）是有意设计的：先让 raster 停用 surface，再销毁窗口，最后 join 线程。调整前先读 ARCHITECTURE §3.8，并跑 `ApplicationTest`。

### 4.3 错误处理

- 全项目**禁用异常和 RTTI**（`-fno-exceptions -fno-rtti`）。可失败的操作返回 `Status` / `StatusOr<T>`，用 `MOMO_RETURN_IF_ERROR` / `MOMO_ASSIGN_OR_RETURN` 传递。
- JS 侧的错误在绑定层转成 JS 异常（`ThrowError` / `ThrowTypeError`）；C++ 调 JS 时，JS 异常收敛为 `Status`。

### 4.4 依赖方向

- engine **不得** include `embedder/` 下的任何文件。engine 需要平台能力时，在 `engine/platform/` 声明纯接口，由 embedder 实现（依赖倒置）。
- **engine 只认识视图**（画面、尺寸、输入、焦点、捕获）。窗口的外观与行为（标题、位置、标题栏、拖动、关闭询问）、剪贴板、打开 URL、输入法属于宿主：embedder 在 `Shell::Create` 的 `on_created` 里往 `hub()` 注册通道处理器（`window_channel.h`、`platform_channels.h`），JS 与宿主直接往返。不要为这类需求往 `Shell::Delegate`、`Engine::Delegate`、`WindowClient` 加方法。
- `v8.h` 只能出现在 `engine/runtime/`、`engine/ui/` 的**实现文件**里；Skia 头只能出现在 `engine/graphics/`、`text/`、`assets/` 的实现文件里。其他模块只看到不含 V8/Skia 类型的接口。
- 平台实现库 `momo_embedder_macos` 只链接 `momo_engine_platform`（接口）和系统 SDK。

### 4.5 契约文件

- `framework/packages/momo/src/ui/momo-ui.d.ts` 是 JS⇄C++ 的接口契约。改绑定必须同步改它，以及 Node mock（`framework/test/mock/`），否则 Node 下的框架测试会和真实引擎行为不一致。
- `StandardCodec` 的线格式在 C++（`engine/common/standard_codec.cc`）和 JS（`services/standard_codec.ts`）各实现一份，必须一致。
- 扩展 C ABI（`momo_extension.h`）已定稿 v1：`MomoHostApi` / `MomoCallApi` **只增不改不删，只能在末尾追加**，扩展用 `struct_size` 判断宿主是否提供某个字段。

### 4.6 Windows 代码

`embedder/windows/` 已写完但**从未在 Windows 上编译过**。修改 `embedder/common/` 的接口（`PlatformWindow`、`WindowDelegate`、`PlatformIntegration`）或 `engine/platform/` 契约时，要手动同步 Windows 实现，并在改动说明里写明"未编译验证"。Windows 目前只声明了 `position`、`pointerCapture` 两项可选窗口能力，其余（隐藏标题栏、拖动区域、移动循环、置顶、平台视图…）对 JS 回复 `unimplemented`。

## 5. 常见改动的步骤

### 5.1 给 `momo:ui` 加一个方法（以 `Canvas.drawArc` 为例）

1. `engine/graphics/display_list.h`：如果是新的绘制指令，加 `OpType` 和 `DisplayListBuilder` 方法；在 `graphics/skia/` 的重放代码里处理它。
2. `engine/ui/painting_bindings.cc`：在 `JsCanvas` 的 `ClassBuilder` 上 `.Method("drawArc", &DrawArc)`，用 `js_args.h` 解码参数。
3. `momo-ui.d.ts`：加类型声明。
4. `framework/test/mock/momo_ui.ts`：让 mock 记录这个调用。
5. 测试：`engine/graphics/graphics_unittests.cc`（C++ 层）和/或 `engine/ui/ui_unittests.cc`（经 JS 调用）；需要像素比对时加 golden。
6. 框架里需要的话，在 `painting/` 或 `rendering/` 使用它。

### 5.2 新增一个平台服务通道（以"显示通知"为例）

1. `embedder/common/platform_services.h`：加纯虚方法（返回 `Status`）。
2. `embedder/macos/mac_platform_integration.mm` 的 `MacServices` 实现它；同步 `embedder/windows/win_services.cc` 和 `embedder/common/testing/mock_platform.h`。
3. `embedder/common/platform_channels.cc`：注册通道 `momo/notification`，handler 跑在 platform 线程，调用 `services_`。
4. 框架 `services/` 加一个基于 `MethodChannel` 的 JS 封装，并从 `services/index.ts` 导出。
5. 测试：`embedder/common/*_unittests.cc`（mock 平台）+ `services_test.ts`。

不需要改 engine，也不需要改 `momo:ui` 绑定：低频能力一律走宿主注册的消息通道。

### 5.2.1 新增一个窗口能力（以"全屏"为例）

1. `embedder/common/platform_window.h`：加可选方法（默认返回 `Unsupported(...)`）和一个 `WindowCapability` 位。
2. `embedder/macos/mac_window.mm` 实现并在 `capabilities()` 里声明；Windows 不支持就不动（JS 收到 `unimplemented`）。
3. `embedder/common/window_channel.cc`：加方法名（`setFullscreen {view, on}`）与能力名；`window_channel_unittests.cc` 加用例。
4. 框架 `services/widget.ts` 的 `Widget` 加方法（宿主推送的变化加事件）。
5. Node mock 的宿主实现（`framework/test/mock/momo_ui.ts` 的 `handleWindowCall`）同步。

### 5.3 新增一个 View（组件）

1. 在 `framework/packages/momo/src/views/`（或 `material/`）实现；需要新布局/绘制时，在 `rendering/` 写 RenderObject。
2. 从对应目录的 `index.ts` 导出。
3. 在 `framework/test/` 用 `testViews` + `ViewTester` 写测试。依赖字形宽度的断言加 `{ ahemText: true }`；只在 mock 下才能观察到的断言加 `{ mockOnly: true }`。
4. `npm test`（Node）+ `ctest -R FrameworkTests`（真实引擎）都要过。

### 5.4 改 embedder 的 macOS 行为

1. 改 `embedder/macos/*.mm`。所有 AppKit 调用都在主线程；raster 线程只会调用 surface 的 `AcquireFrame` / `present` / `FallBackToSoftware`。
2. `momo_embedder_macos_unittests` 覆盖窗口、键码映射、surface、vsync；需要显示器的用例在沙箱里会跳过，所以**这类改动必须在真机上手动验证一次**。

## 6. 测试：什么时候跑什么

| 测试 | 命令 | 覆盖 | 什么时候跑 |
|---|---|---|---|
| base 单测 | `ctest -R 'EventLoop\|Object\|Pipeline\|TaskUtil\|WorkerPool'` | 事件循环、延迟析构、背压 | 改 `base/` |
| engine 单测 | `ctest -R 'Shell\|Animator\|Rasterizer\|Hub\|UiBindings\|Golden'` | Shell 时序、背压、绑定、golden | 改 `engine/` |
| embedder 单测 | `ctest -R 'Application\|AppConfig\|AssetResolver\|Mac'` | 启动/关闭顺序（mock 平台）、macOS 实现 | 改 `embedder/` |
| 扩展 | `ctest -R 'Extension'` | 加载、隔离、端到端、回调线程、ABI 符号、SDK 产物独立构建 | 改扩展宿主或 SDK |
| 框架（Node） | `cd framework && npm test` | 全部 TS 测试 + counter/todo 端到端 | 改 `framework/` |
| 框架（真实引擎） | `ctest -R FrameworkTests` | 同一组 TS 测试跑在 `momo_headless` 上 | 改绑定或框架 |
| 冒烟 | `ctest -R HeadlessSmoke` | 无窗口完整跑一个 bundle 并截图 | 任何引擎改动 |
| 类型检查 | `cd framework && npx tsc --noEmit` | 框架、测试与示例 | 改 TS |

提交前跑一次完整的 `ctest -j8` 和 `npm test`，两者加起来几分钟。

只跑名字里包含某个子串的框架用例：先执行 `npm run build:tests`，再执行 `node out/test/node_main.mjs <子串>`。

golden 图需要更新时（确认是预期内的视觉变化）：

```bash
MOMO_UPDATE_GOLDENS=1 ctest -R Golden
```

然后在改动说明里附上新旧图片的对比。

## 7. 调试技巧

- **日志**：`MOMO_LOG(INFO|WARNING|ERROR|VERBOSE) << ...`，输出到 stderr。Debug 构建会打印 `VERBOSE`（生命周期切换、metrics、surface 绑定等），Release 只打印 `INFO` 及以上。从终端启动 `Momo.app/Contents/MacOS/Momo` 就能看到。
- **线程断言**：Debug 构建中，`MOMO_DCHECK_ON_AFFINITY_THREAD()` / `MOMO_DCHECK_RUNS_ON(runner)` 失败会打印"Called off the object's affinity thread"。看到它，就说明某处跨线程直接调用了对象。
- **JS**：`--dev` 启动后用 Chrome DevTools 连 `localhost:9229`；`--inspect-brk` 在执行 bundle 前停下。
- **渲染问题**：`MOMO_SOFTWARE_RENDERING=1` 切到 CPU 光栅。如果问题消失，就是 Metal 路径的问题。
- **不开窗口复现**：`momo_headless --bundle=x.js --width=800 --height=600 --screenshot=out.png`。bundle 通过 `momo/test` 通道调 `screenshot` / `done`，用法见 `embedder/headless/headless_main.cc` 开头的注释。
- **ThreadSanitizer**：`cmake -B out/tsan -DMOMO_ENABLE_TSAN=ON`。注意 sanitizer 构建里没有 V8（见 §8），JS 相关路径用占位驱动代替。

## 8. 已知的坑

- **V8 预编译包要求 macOS ≥ 13**，而项目部署目标是 12，所以链接时会有 `built for newer version 13.0` 警告。这是已知问题，不影响运行（docs/09 §2.1）。
- **sanitizer 构建没有 V8**：V8 包里的 partition_alloc 与 ASan/TSan 冲突。此时 `engine/ui` 只编译 `app_driver_no_js.cc`，JS 相关测试不会被编译进来。
- **沙箱里会跳过 9 项测试**：Inspector 网络用例需要绑定端口；Metal 和 CVDisplayLink 用例需要 GPU 或显示器。只在沙箱里测通过，不代表这些路径没问题。
- **`nextDrawable` 可能返回 nil**：此时直接丢帧，不能阻塞等待。
- **AppKit 的键盘怪癖**：按住 Command 时 `-[NSApplication sendEvent:]` 会吞掉 `keyUp`。`MomoContentView` 用本地事件监视器补发（Flutter macOS 也是这么做），并在 `keyUp:` 里按事件对象去重。`flagsChanged` 事件不能访问 `characters`（会抛 ObjC 异常），修饰键只看 `modifierFlags`。
- **hot restart 不保留 JS 状态**，并且会销毁整个 JS 运行时。所以绑定层中所有异步回调都要用 `WeakPtr<Impl>` 判活（见 `ui_impl.h`）。
- **框架里没有 DOM/Node API**：没有 `setTimeout`，要用 `SchedulerBinding.instance.scheduleDelayed`。

## 9. 文档维护约定

每类信息只在一个地方维护，其他地方用链接引用：

| 信息 | 唯一来源 |
|---|---|
| 怎么用（安装、API 速查、打包） | README.md |
| 代码现在怎么工作 | ARCHITECTURE.md |
| 为什么这样设计、规划中的设计 | `docs/0x-*.md` |
| 每项功能的完成状态 | `docs/11-roadmap.md` |
| 某个类的线程约定与生命周期 | 该类头文件开头的注释 |
| 维护流程与规则 | 本文 |

规则：

- 改了代码里的类名、文件名或流程，要同步改 ARCHITECTURE.md 和相关的 `docs/`。`docs/` 中写的类名必须在代码中真实存在；尚未实现的设计要明确标注"规划"或"未实现"。
- 各目录下的 `README.md` 只写"这个目录是什么、怎么跑"，不要另维护一份进度状态，状态统一写在 roadmap。
- ARCHITECTURE.md 的图是 `docs/images/*.svg`，手写的 SVG，直接编辑文本即可。

## 10. 技术债与下一步

按优先级排列，供接手后挑选：

1. **CI**：目前只有本地构建。建议先在 macOS arm64 上跑 `ctest` 和 `npm test`，再加 TSan 任务。
2. **Windows**：平台代码从未编译过，ANGLE GPU 后端未实现，V8 也没有 Windows 预编译包。
3. **真机验证**：沙箱跳过的 9 项测试，以及 Metal 回退、IME 候选窗位置、多显示器切换，需要在真机上定期手动验证。
4. **raster cache**：设计中有，但没有实现，每帧完整重放所有 DisplayList。
5. **`PlatformIntegration::displays()` 返回的信息过多**：目前只用到主显示器的刷新率，`DisplayInfo` 的其余字段没有用处。可以收窄为 `primary_refresh_rate()`。这要同时改 Windows 实现和 mock，所以暂未处理。
6. 多窗口、无障碍、崩溃上报、性能浮层：见 docs/11 M6。
