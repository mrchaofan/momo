# 02 · Engine 层设计

Engine 是框架的核心 C++ 层:向上为 JS 提供 `momo:*` 原生 API,向下只依赖自己声明的平台契约 `engine/platform/`(纯接口,由 embedder 实现并注入)。对标 Flutter Engine(shell/runtime/lib/ui/flow)。

## 1. 目录结构与模块

```
engine/
├── CMakeLists.txt
├── platform/        # 平台契约(纯接口 + POD): RenderSurface、VsyncSource、输入、IME、PlatformServices、ViewportMetrics
├── common/          # 无 V8/Skia 的公共设施: Settings、TaskRunners、MessageChannelHub、StandardCodec、JSON、AppLayout(应用在磁盘上的样子)
├── shell/           # 协调中枢: Shell、Engine、Animator、Rasterizer、ThreadHost
├── runtime/         # V8 集成: isolate/context 管理、绑定基建、模块加载、inspector
├── ui/              # momo:ui 原生实现: window(全局帧回调)、views(视图)、Canvas、SceneBuilder、事件入口
├── graphics/        # DisplayList、LayerTree、Skia 封装、GPU 后端(compositor)
├── text/            # SkParagraph 封装: 字体管理、段落排版
├── assets/          # 资源与图片: AssetManager、图片解码(codec)、缓存
└── native_host/     # 原生扩展的宿主能力表(MomoHost)的实现
```

每个模块一个静态库 `momo_engine_<module>`;线程/任务/对象/Status 等基础设施来自顶层 `base/`(docs/12)。模块间依赖:

```
shell → ui, native_host, graphics, common, platform, base
ui    → runtime(绑定基建), graphics, text, assets, common, platform, base
        (无 V8 构建: ui 只含占位驱动, 不依赖 runtime)
runtime → base, (v8)
native_host → graphics, common, base, (momo_ext_sdk)
text, assets → graphics, base, (skia)
graphics → base, (skia)
common, platform → base
(单向无环:Settings/TaskRunners 在 common, 供 ui/native_host/shell 共用;AppDriver 接口与其两种实现都在 ui)
```

## 2. shell/ — 协调中枢

| 类 | 线程 | 职责 |
|---|---|---|
| `Shell` | 创建于 platform | 组装所有子系统;生命周期根对象;跨线程调用的安全入口 |
| `TaskRunners`(common) | — | 四个 `TaskRunner`(platform/ui/raster/io)的集合,见 docs/08 |
| `ThreadHost` | 宿主线程 | 创建/join ui、raster、io 三个具名线程并产出 TaskRunners;由宿主(embedder/测试)持有,见 docs/08 §2 |
| `Animator` | ui | 帧调度:请求 VSync → 驱动 AppDriver 出帧;一帧收集各视图的 LayerTree 成一个 `FrameItem` 入 Pipeline;背压;由 Engine 持有 |
| `Rasterizer` | raster | 接收 FrameItem → 按 `LayerTree::view_id` 画到对应视图的 surface(`RenderBackend::Draw`)→ present;每个视图一份最近一帧, 后端按 GPU 设备共享(同一设备上的全部视图共用一个后端 = 整个应用一份 GrDirectContext) |
| `Engine`(内部类) | ui | ui 线程侧的根:持有各视图的 ViewportMetrics/焦点、Animator 与 `ui::AppDriver`(JS 驱动或占位驱动, 由 ui 模块的构建配置决定, 见 `engine/ui/app_driver.h`);`StartRuntime()` 创建 JS 运行时并执行 bundle(只一次), 运行时随 Engine 析构而销毁;没有运行时的时候不出帧 |

启动分两阶段。`Shell::Create()`:在 platform 线程构造 → 向 raster 线程投递 Rasterizer 初始化 → 向 ui 线程投递 Engine 的构造(Animator 用定时器 vsync, **没有 JS 运行时**)→ 不等待, 立即返回 embedder。宿主随后在 `hub()` 上注册通道处理器、`AttachView` 绑定已有窗口, 再调用 `Shell::Run(done)`(只能一次):ui 线程 `Engine::StartRuntime`(建 AppDriver/V8 并执行 bundle, 再把已有视图的状态交给它)→ 结果经 `done` 回到 platform 线程;bundle 失败时引擎没有运行时、不出帧, 是否退出由宿主决定。platform 线程从不等待 bundle 执行。JS 运行时在进程里只有这一个, 没有热重启(docs/04 §8)。`Shutdown` 的顺序(原生扩展的代码随 JS 运行时结束, 所以引擎对扩展的调用先结束, docs/07 §4):关闭扩展的宿主能力表 → 销毁全部平台视图并清空工厂 → ui 线程销毁 JS 运行时(扩展登记的清理函数在这一步执行), 再是 Engine 其余部分(同步汇合: 此前投递给 JS 的事件都先执行完;JS 不返回时就等着, 不强行终止它的执行)→ raster 释放 GPU 资源。宿主在消息循环结束之后才调用它(docs/01 §4)。帧时钟是应用级的:宿主以 `SetVsyncSource(source)` 给出唯一的 VsyncSource(Shell 在 platform 线程持有, ui 侧 Animator 改用 `PlatformVsyncWaiter`;没有则用定时器), 与视图的绑定无关。`AttachView(view_id, delegate, surface)` 由 Shell 在 platform 线程持有窗口 delegate;`DetachView(view_id)` 由 raster 同步解绑 surface;`Shutdown` 释放 VsyncSource。

### 多视图(多窗口)

一个 Shell(一个 JS 运行时)驱动多个视图,每个视图对应宿主的一个原生窗口(`engine/platform/view.h`:`ViewId`;视图 0 = 隐式视图, 即宿主启动时创建、在 `Shell::Run` 之前绑定(因此 bundle 执行前就存在)的窗口, 与其他视图一样可被移除;引擎自己不预置视图)。

- **共享**:JS 状态、帧时钟(一个 Animator、一个 VsyncSource;跟随哪块显示器由宿主决定)、平台视图表、纹理表、扩展的宿主能力表。指针序列的接收方(捕获)由宿主与框架决定, 引擎按视图原样转发输入(ARCHITECTURE §3.10)。
- **按视图**:surface 与 GPU 后端、ViewportMetrics、焦点、指针/键盘事件、合成状态。
- **引擎不管窗口**:窗口的创建与关闭、标题、光标、位置、标题栏都属于宿主(embedder 的 `momo/window` 通道, docs/03 §5)。宿主建好窗口后以自己分配的 id `AttachView`(JS 收到 `onViewsChanged`), 关闭时 `DetachView`。引擎的接口(`Shell::Delegate`、`ui::WindowClient`)里只有视图本身的事。
- **帧路径**:一次 `onDrawFrame` 中各视图各自 `view.render(scene)`(每视图每帧至多一次),Animator 把它们打包成一个 `FrameItem`,raster 按 view_id 分别光栅化与呈现。
- **输入法**:由宿主的 `momo/textinput` 交给当前获得焦点的窗口。

## 3. runtime/ — V8 集成(详见 docs/04)

| 组件 | 职责 |
|---|---|
| `JsRuntime`(`js_runtime.h`) | 一个 Node 环境(v8::Isolate + 主 Context + uv 循环):创建/销毁、执行主模块、排空 microtask;`RegisterNativeModule(specifier, factory)` 注册引擎的原生模块(`momo:ui`、`momo:services`);运行时自带的 JS 模块(`momo:framework`、内置扩展, 在 `Settings::builtin_dir`)与 `.node`(原生扩展)的 import 由模块钩子支持(docs/04 §4);JS 异常转 `Status`;编译缓存;等待调试器。头文件含 V8 类型, 只被 runtime 与 ui 的实现文件包含 |
| `Wrappable` + `ClassBuilder<T>` | 把 C++ 对象暴露为 JS 类(类型校验不依赖 RTTI,GC 弱回调释放) |
| `js_args.h` | 绑定函数的参数解码(`ArgNumber`、`ArgWrapped<T>`…) |
| `NodeBindings` | libuv 循环接到 ui 线程(poll 线程 + `uv_sem`,对照 Electron);`--dev` 时 Node 的 inspector 供 Chrome DevTools 连接 |

消息通道 `MessageChannelHub` 不在 runtime,而在 `common/`(不依赖 V8)。

## 4. ui/ — momo:ui 的原生实现

这是 JS 框架与 engine 的主要同步边界,对标 `dart:ui`:

| JS 可见对象 | C++ 实现(`engine/ui/`) | 说明 |
|---|---|---|
| `window` / `views` | `platform_dispatcher_binding.cc`(`UiBindings::Impl`)→ `WindowClient`(由 `Engine` 实现) | `window`: 全局帧调度与帧回调;每个视图: 尺寸、DPR、焦点、输入回调, `view.render(scene)` 提交一帧 |
| `Canvas` / `Paint` / `Path` / `Gradient` | `painting_bindings.cc`(`JsCanvas` 等)→ `graphics::DisplayListBuilder` | JS 调 drawRect/drawPath/drawParagraph,记录为 DisplayList |
| `PictureRecorder` / `Picture` | 同上(`JsPictureRecorder` / `JsPicture`) | Picture = 不可变 DisplayList |
| `SceneBuilder` / `Scene` | `scene_bindings.cc`(`JsSceneBuilder` / `JsScene`)→ `graphics::LayerTree` | pushOffset/pushClipRect/addPicture,组装图层树 |
| `ParagraphBuilder` / `Paragraph` | `text_bindings.cc` → `text::Paragraph` | 排版与测量(layout/getBoxesForRange) |
| `Image` / `Codec` / `loadAsset` | `image_bindings.cc` → `assets::` | 解码在 io 线程,JS 拿到 Promise |
| `momo:services` | `services_bindings.cc` → `MessageChannelHub` | 异步消息通道;另有 `assetsDir`(应用资源目录)与 `nativeHost`(宿主能力表的地址, 原生扩展用, docs/07 §3) |
| 事件入口 | `UiBindings`(`ui_bindings.h`) | `BeginFrame/DrawFrame/PointerPacket/KeyData/MetricsChanged…` 调用 `window.on*` 回调 |

`ui` 对 shell 只暴露不含 V8 的 `app_driver.h`:`AppDriver`(`JsAppDriver` 或 `PlaceholderDriver`)。

设计原则:**JS ↔ C++ 边界只传"描述"不传"像素"**;所有绘制 API 只做记录(DisplayList),重活全部在 raster/io 线程。

## 5. graphics/ — 绘制管线(详见 docs/05)

`DisplayList`(可重放绘制指令)、`LayerTree/Layer`(合成树:ContainerLayer、OffsetLayer、TransformLayer、ClipRectLayer、ClipRRectLayer、OpacityLayer、PictureLayer、TextureLayer;每层 `Preroll(PrerollContext&)` + `Paint(PaintContext&)` 两遍遍历)、光栅实现 `RasterizeLayerTree`(软件)与 `MetalRasterizer`(`gpu/`)。按 surface 种类选择后端的 `RenderBackend` 在 `shell/render_backend.h`(Metal / 软件 / Null;ANGLE-GL 未实现)。

## 6. text/ 与 assets/

- `FontCollection`:系统字体枚举(经 platform 接口拿平台字体管理器)+ 应用内嵌字体;回退链。
- `ParagraphBuilder/Paragraph`:薄封装 SkParagraph。
- `AssetManager`:统一资源定位(资源目录是应用的 `assets/`, 由 `common/app_layout.h` 的 `LocateApp` 给出);`image_decoder.h`(`MultiFrameCodec` 等):io 线程解码,raster 线程首次使用时惰性上传。图片缓存(LRU)在框架层 `painting/image_provider.ts` 的 `ImageCache`,引擎不做缓存。

## 7. native_host/ — 原生扩展的宿主能力表(详见 docs/07)

原生扩展是 Node.js 原生插件(`.node`), 由 JS `import` 加载;引擎不知道有哪些扩展, 也不加载任何东西。`NativeHost`(`native_host.cc`)只实现宿主能力表 `MomoHost`(`extensions/sdk/include/momo/momo_host.h`):把任务投到 platform 线程、外部纹理(`TextureRegistry`)、平台视图工厂(`PlatformViewRegistry`)。本模块不含 V8;表的地址经 `momo:services` 的 `nativeHost` 交给 JS, 再由 JS 传给插件。`Close()`(`Shell::Shutdown` 的第一步)之后表里每个函数返回 `kMomoErrClosed`, 已投递而未执行的平台任务被丢弃。扩展推送纹理新帧时, 帧请求经 `TextureRegistry` 的监听者交给 Engine。

## 8. 错误处理与所有权

- 全层禁异常:可失败操作返回 `momo::StatusOr<T>`(base 提供,参考 absl);JS 侧错误转为 JS exception 抛出(由绑定基建统一完成)。
- 所有权:`std::unique_ptr` 为默认;跨线程共享的**不可变数据**(DisplayList、Image)可用 `std::shared_ptr`;有线程归属的对象(GPU 资源、平台对象如 VsyncSource)继承 `momo::Object`,以 `ObjectPtr` 独占持有,释放即 `DeleteLater`,由归属线程的 EventLoop 在安全点析构(docs/08 §3)。
