# 00 · 架构总览

本文是 Momo 框架的入口文档,给出整体分层、核心概念、一帧的生命周期,以及各设计文档的阅读顺序。

## 1. 目标与非目标

**目标**

- 开发者用 JS/TS 编写 Windows / macOS 桌面应用,UI 完全自绘,双端像素一致。
- 产物为原生可执行程序:原生入口启动 → 创建窗口与渲染表面 → 在合适时机创建 V8 执行环境 → 加载 JS bundle → 运行应用。
- 向 JS 暴露一组稳定的底层 API(命名空间 `momo:ui`、`momo:services` 等,对标 `dart:ui` + Services),View 体系(组件)、app 与 Widget(应用与窗口对象)在 JS 侧实现。
- 提供三方扩展机制:扩展可以用原生代码(稳定 C ABI)借助 engine 层能力直接调用平台原生 API,再以 JS 包的形式提供给应用开发者。

**非目标(首期)**

- 不做移动端 / Web;不做系统控件混排(Platform View);不追求与 Flutter API 兼容,只借鉴其分层。

## 2. 分层与职责

按六个核心职责划分:**UI 描述与更新 → 布局 → 绘制描述 → 场景处理 → 图形后端 → 平台宿主**;运行时、帧调度、输入、平台服务作为支撑子系统。

| 层 | 语言 | 目录 | 职责 | 对标 Flutter |
|---|---|---|---|---|
| Framework | TS | `framework/` | View/Element/RenderObject 三树、布局、绘制组织、手势、动画、调度 | Framework (Dart) |
| Engine | C++ | `engine/` | V8 运行时、`momo:ui` 原生绑定、DisplayList、LayerTree 合成、Skia 光栅化、文本排版、图片解码、扩展宿主、Shell 协调 | Engine (C++) |
| Embedder | C++ | `embedder/` | 可执行程序入口、消息循环、生命周期、Engine 装配与驱动;平台抽象(`common/`)与 Win/Mac/headless 平台实现(窗口、Surface、输入、IME、剪贴板、VSync) | shell/platform/{darwin,windows} + runner |
| Base | C++ | `base/` | 全项目基础库:对象模型(Object/DeleteLater/WeakPtr)、EventLoop/TaskRunner/线程、Status/logging(docs/12) | fml / Chromium base |

依赖规则(编译期强制,CMake 不提供反向链接):

```
framework(JS)  ⇅ 仅通过 momo:* 绑定与消息通道
engine         → base(+ Skia/V8);自己声明平台契约 engine/platform/(纯接口),不依赖 embedder
embedder       → engine(含 engine/platform 契约)+ 操作系统 SDK;实现契约并注入
base           → 仅标准库(所有 C++ 层的最底层)
```

关键点:**engine 只认识 `engine/platform/` 里的纯虚接口**(`RenderSurface`、`VsyncSource`、输入事件类型、视图尺寸…),embedder 负责实例化平台实现并注入(依赖倒置:接口归使用方 engine 所有,与 Flutter 的 `Surface`/`VsyncWaiter`/`PlatformView` 位于 engine 内一致);窗口抽象 `PlatformWindow`、工厂 `PlatformIntegration` 只有 embedder 用,放在 `embedder/common/`。这保证 engine 可以脱离真实平台做 headless 测试,也让移植新平台 = 实现一组接口 + 写一个新 embedder。engine 只认识**视图**(画面、尺寸、输入);窗口的外观与行为、剪贴板、输入法等由 embedder 在消息通道上注册处理器提供(`momo/window`、`momo/clipboard`…), 不进入 engine 的接口——新增这类能力不需要改 engine(同 Flutter 的 engine/embedder 分工)。

## 3. 核心运行时对象

```
embedder::Application (每进程一个)
 └─ 拥有 ──► engine::Shell (每引擎实例一个)
                                    ├─ TaskRunners {platform, ui, raster, io}
                                    ├─ runtime::JsRuntime (V8 Isolate + Context)
                                    │    └─ ui::UiBindings (momo:ui 等绑定)
                                    ├─ Animator (帧调度, 连接 VsyncSource)
                                    ├─ Rasterizer (raster 线程, 持有 GPU Context)
                                    └─ extension::ExtensionHost (扩展注册与生命周期)
 └─ embedder::PlatformWindow (由 embedder 创建; 其 RenderSurface/VsyncSource 经 AttachView 交给 Shell)
```

## 4. 线程模型(详见 docs/08)

与 Flutter 相同的四线程(Task Runner)模型:

- **Platform 线程**:操作系统主线程,运行原生消息循环,所有平台 API 调用在此。
- **UI 线程**:运行 V8,执行 JS 框架代码,产出 LayerTree。
- **Raster 线程**:持有 GPU 上下文,重放 DisplayList,Skia 光栅化并 present。
- **IO 线程**:资源读取、图片解码(可带共享 GPU 上下文做纹理上传)。

## 5. 一帧的生命周期

```
VSync(platform) ─► Animator.beginFrame(ui 线程)
  ─► JS: onBeginFrame → 框架 build/layout/paint → 生成 DisplayList
  ─► JS: SceneBuilder 组装 LayerTree → view.render(scene)(每个视图一棵)
  ─► Shell 将 LayerTree 投递到 raster 线程
  ─► Rasterizer: preroll → Skia 绘制到 RenderSurface → present
```

## 6. JS 与原生的三条通道

1. **momo:ui 同步绑定**(高频、低延迟):Canvas/DisplayList 记录、SceneBuilder、窗口事件、输入事件。V8 FunctionTemplate 直接绑定,见 docs/04。
2. **消息通道 MessageChannel**(低频、异步):平台服务(剪贴板、对话框、菜单…)与扩展通信,二进制编码,对标 Flutter 的 Platform Channel,见 docs/04 与 docs/03。
3. **扩展 Host API**(三方原生扩展):稳定 C ABI,扩展可注册 JS 可见模块、收发消息、访问窗口句柄/GPU 上下文等 engine 能力,见 docs/07。

## 7. 文档目录

| 文档 | 内容 |
|---|---|
| `01-embedder.md` | 可执行程序层:入口、生命周期、消息循环、Engine 装配 |
| `02-engine.md` | 引擎层:shell/runtime/ui/graphics/text/assets/extension 模块设计 |
| `03-platform.md` | 平台抽象接口与 Windows/macOS 实现要点 |
| `04-js-runtime.md` | V8 集成、绑定机制、模块系统、Inspector 调试 |
| `05-rendering-pipeline.md` | DisplayList、LayerTree、Rasterizer、GPU 后端选择 |
| `06-framework-js.md` | TS 框架层:三树模型、布局协议、手势、动画、包结构 |
| `07-extension-system.md` | 三方扩展机制:C ABI、JS 封装、分发与加载 |
| `08-threading-model.md` | Task Runner、线程间通信、对象生命周期与线程归属 |
| `09-build-and-deps.md` | CMake 组织、第三方依赖(Skia/V8)管理、JS 构建、打包 |
| `10-coding-standards.md` | C++ 规范(无异常/RTTI、错误处理、所有权)、TS 规范 |
| `11-roadmap.md` | 里程碑分治与验收标准 |
| `12-base.md` | base 基础库:对象模型(Object/DeleteLater/WeakPtr)、EventLoop 与线程、任务组合 |
