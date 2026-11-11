# 02 · Engine 层设计

Engine 是框架的核心 C++ 层:向上为 JS 提供 `momo:*` 原生 API,向下只依赖自己声明的平台契约 `engine/platform/`(纯接口,由 embedder 实现并注入)。对标 Flutter Engine(shell/runtime/lib/ui/flow)。

## 1. 目录结构与模块

```
engine/
├── CMakeLists.txt
├── platform/        # 平台契约(纯接口 + POD): RenderSurface、VsyncSource、输入、IME、PlatformServices、ViewportMetrics
├── common/          # 无 V8/Skia 的公共设施: Settings、TaskRunners、MessageChannelHub、StandardCodec、JSON
├── shell/           # 协调中枢: Shell、Engine、Animator、Rasterizer、ThreadHost
├── runtime/         # V8 集成: isolate/context 管理、绑定基建、模块加载、inspector
├── ui/              # momo:ui 原生实现: View/window、views/createView、Canvas、SceneBuilder、事件入口
├── graphics/        # DisplayList、LayerTree、Skia 封装、GPU 后端(compositor)
├── text/            # SkParagraph 封装: 字体管理、段落排版
├── assets/          # 资源与图片: AssetManager、图片解码(codec)、缓存
└── extension/       # 扩展宿主: C ABI 实现、模块注册表、扩展生命周期
```

每个模块一个静态库 `momo_engine_<module>`;线程/任务/对象/Status 等基础设施来自顶层 `base/`(docs/12)。模块间依赖:

```
shell → ui, extension, graphics, common, platform, base
ui    → runtime(绑定基建), extension, graphics, text, assets, common, platform, base
        (无 V8 构建: ui 只含占位驱动, 不依赖 runtime)
runtime → base, (v8)
extension → assets, graphics, common, base, (momo_ext_sdk)
text, assets → graphics, base, (skia)
graphics → base, (skia)
common, platform → base
(单向无环:Settings/TaskRunners 在 common, 供 ui/extension/shell 共用;AppDriver 接口与其两种实现都在 ui)
```

## 2. shell/ — 协调中枢

| 类 | 线程 | 职责 |
|---|---|---|
| `Shell` | 创建于 platform | 组装所有子系统;生命周期根对象;跨线程调用的安全入口 |
| `TaskRunners`(common) | — | 四个 `TaskRunner`(platform/ui/raster/io)的集合,见 docs/08 |
| `ThreadHost` | 宿主线程 | 创建/join ui、raster、io 三个具名线程并产出 TaskRunners;由宿主(embedder/测试)持有,见 docs/08 §2 |
| `Animator` | ui | 帧调度:请求 VSync → 驱动 AppDriver 出帧;一帧收集各视图的 LayerTree 成一个 `FrameItem` 入 Pipeline;背压;由 Engine 持有 |
| `Rasterizer` | raster | 接收 FrameItem → 按 `LayerTree::view_id` 画到对应视图的 surface(`RenderBackend::Draw`)→ present;每个视图一份后端与最近一帧 |
| `Engine`(内部类) | ui | ui 线程侧的根:持有各视图的 ViewportMetrics/焦点、Animator 与 `ui::AppDriver`(JS 驱动或占位驱动, 由 ui 模块的构建配置决定, 见 `engine/ui/app_driver.h`) |

`Shell::Create()` 流程:在 platform 线程构造 → 向 raster 线程投递 Rasterizer 初始化 → 向 ui 线程投递 Engine 初始化(Animator 用定时器 vsync, 再建 AppDriver/V8 并执行 bundle)→ 两者就绪后返回 embedder。`AttachView(view_id, delegate, surface, vsync)` 由 Shell 在 platform 线程持有窗口 delegate 与 VsyncSource,ui 侧 Animator 改用 `PlatformVsyncWaiter`;`DetachView(view_id)`/`Shutdown` 先在 platform 线程释放 VsyncSource(帧时钟改用其他视图的来源或定时器),最后 raster 同步解绑 surface。

### 多视图(多窗口)

一个 Shell(一个 JS 运行时)驱动多个视图,每个视图对应宿主的一个原生窗口(`engine/platform/view.h`:`ViewId`,视图 0 = 主窗口)。

- **共享**:JS 状态、帧时钟(一个 Animator;VsyncSource 优先用视图 0 的)、平台视图表、纹理表、扩展。
- **按视图**:surface 与 GPU 后端、ViewportMetrics、焦点、指针/键盘事件、窗口标题/光标/最小尺寸、合成状态。
- **创建/关闭**:JS `createView(options)` → Engine → Shell 在 platform 线程分配 id → `Shell::HostDelegate::CreateView`(embedder 的 Application 建窗口并 `AttachView`)→ 结果回到 ui 线程结算 Promise(此前 JS 已收到 `onViewsChanged`)。`view.close()` → `HostDelegate::CloseView`(`DetachView` 后销毁窗口)。用户点非主窗口的关闭按钮先问 `view.onCloseRequested`。
- **帧路径**:一次 `onDrawFrame` 中各视图各自 `view.render(scene)`(每视图每帧至多一次),Animator 把它们打包成一个 `FrameItem`,raster 按 view_id 分别光栅化与呈现。
- **输入法**:`momo/textinput` 发往当前获得焦点的窗口。热重启时其他窗口一并关闭。
- 扩展 `get_window_handle(view_id)` 对所有视图有效。

## 3. runtime/ — V8 集成(详见 docs/04)

| 组件 | 职责 |
|---|---|
| `JsRuntime`(`js_runtime.h`) | 公共头,不含 V8 类型:创建/销毁运行时、执行主模块、排空 microtask |
| `JsRuntimeImpl`(`js_runtime_impl.h`) | v8::Isolate + 主 Context;ESM 模块解析(`ResolveModule`);`RegisterNativeModule(specifier, factory)` 注册 `momo:*` / `ext:*` synthetic module;JS 异常转 `Status`;V8 code cache |
| `Wrappable` + `ClassBuilder<T>` | 把 C++ 对象暴露为 JS 类(类型校验不依赖 RTTI,GC 弱回调释放) |
| `js_args.h` | 绑定函数的参数解码(`ArgNumber`、`ArgWrapped<T>`…) |
| `NodeBindings` | libuv 循环接到 ui 线程(poll 线程 + `uv_sem`,对照 Electron);`--dev` 时 Node 的 inspector 供 Chrome DevTools 连接 |

消息通道 `MessageChannelHub` 不在 runtime,而在 `common/`(不依赖 V8,platform 服务与扩展共用)。

## 4. ui/ — momo:ui 的原生实现

这是 JS 框架与 engine 的主要同步边界,对标 `dart:ui`:

| JS 可见对象 | C++ 实现(`engine/ui/`) | 说明 |
|---|---|---|
| `window` | `window_binding.cc`(`UiBindings::Impl`)→ `WindowClient`(由 `Engine` 实现) | 尺寸、DPR、焦点;`window.render(scene)` 提交一帧 |
| `Canvas` / `Paint` / `Path` / `Gradient` | `painting_bindings.cc`(`JsCanvas` 等)→ `graphics::DisplayListBuilder` | JS 调 drawRect/drawPath/drawParagraph,记录为 DisplayList |
| `PictureRecorder` / `Picture` | 同上(`JsPictureRecorder` / `JsPicture`) | Picture = 不可变 DisplayList |
| `SceneBuilder` / `Scene` | `scene_bindings.cc`(`JsSceneBuilder` / `JsScene`)→ `graphics::LayerTree` | pushOffset/pushClipRect/addPicture,组装图层树 |
| `ParagraphBuilder` / `Paragraph` | `text_bindings.cc` → `text::Paragraph` | 排版与测量(layout/getBoxesForRange) |
| `Image` / `Codec` / `loadAsset` | `image_bindings.cc` → `assets::` | 解码在 io 线程,JS 拿到 Promise |
| `momo:services` | `services_bindings.cc` → `MessageChannelHub` | 异步消息通道 |
| `ext:*` | `extension_bindings.cc` | 扩展在 on_attach 中注册的函数 |
| 事件入口 | `UiBindings`(`ui_bindings.h`) | `BeginFrame/DrawFrame/PointerPacket/KeyData/MetricsChanged…` 调用 `window.on*` 回调 |

`ui` 对 shell 只暴露不含 V8 的 `app_driver.h`:`AppDriver`(`JsAppDriver` 或 `PlaceholderDriver`)。

设计原则:**JS ↔ C++ 边界只传"描述"不传"像素"**;所有绘制 API 只做记录(DisplayList),重活全部在 raster/io 线程。

## 5. graphics/ — 绘制管线(详见 docs/05)

`DisplayList`(可重放绘制指令)、`LayerTree/Layer`(合成树:ContainerLayer、OffsetLayer、TransformLayer、ClipRectLayer、ClipRRectLayer、OpacityLayer、PictureLayer、TextureLayer;每层 `Preroll(PrerollContext&)` + `Paint(PaintContext&)` 两遍遍历)、光栅实现 `RasterizeLayerTree`(软件)与 `MetalRasterizer`(`gpu/`)。按 surface 种类选择后端的 `RenderBackend` 在 `shell/render_backend.h`(Metal / 软件 / Null;ANGLE-GL 未实现)。

## 6. text/ 与 assets/

- `FontCollection`:系统字体枚举(经 platform 接口拿平台字体管理器)+ 应用内嵌字体;回退链。
- `ParagraphBuilder/Paragraph`:薄封装 SkParagraph。
- `AssetManager`:统一资源定位(来自 embedder 的 AssetResolver);`image_decoder.h`(`MultiFrameCodec` 等):io 线程解码,raster 线程首次使用时惰性上传。图片缓存(LRU)在框架层 `painting/image_provider.ts` 的 `ImageCache`,引擎不做缓存。

## 7. extension/ — 扩展宿主(详见 docs/07)

`ExtensionHost`(`extension_host.cc`:manifest 解析、加载 .dll/.dylib、版本握手、生命周期,并实现 C ABI 函数表 `MomoHostApi` 中不涉及 JS 的部分)。扩展注册的 `ext:*` 模块与函数由 `ui/extension_bindings.cc` 在 JS 运行时创建时安装(实现 `MomoCallApi`)。

## 8. 错误处理与所有权

- 全层禁异常:可失败操作返回 `momo::StatusOr<T>`(base 提供,参考 absl);JS 侧错误转为 JS exception 抛出(由绑定基建统一完成)。
- 所有权:`std::unique_ptr` 为默认;跨线程共享的**不可变数据**(DisplayList、Image)可用 `std::shared_ptr`;有线程归属的对象(GPU 资源、平台对象如 VsyncSource)继承 `momo::Object`,以 `ObjectPtr` 独占持有,释放即 `DeleteLater`,由归属线程的 EventLoop 在安全点析构(docs/08 §3)。
