# 11 · 实现路线图(分治方案)

按里程碑垂直切片推进:每个里程碑都产出**可运行、可验收**的东西,而不是水平地先写完某一层。每条任务可独立成 issue。

## M0 · 骨架与地基(约 2-3 周)

目标:仓库可构建,双端弹出原生空窗口,四线程就绪。

- [~] CMake 骨架 ✅;CMakePresets 与 CI 未建(本仓库仅本地构建)
- [x] base:Status/StatusOr、logging、TaskRunner/EventLoop(MessagePump)、Object(DeleteLater 生命周期)、WeakPtr、task_util、macros(docs/12)
- [x] 平台接口头文件定稿(engine/platform 契约 + embedder/common 平台抽象)(M3 补完 IME 协议、M2 补完 GPU 帧)
- [~] embedder/macos ✅;embedder/windows 代码已写完, **未在 Windows 上编译验证**
- [x] embedder:入口、Application、生命周期状态机、TaskRunners 启动与有序关闭(含单实例/WM_COPYDATA, Windows 未验证)
- [~] 验收:Mac ✅(单测 + TSan 干净);Windows 待在真机验证

## M1 · 第一帧像素(约 3-4 周)

目标:JS 调 Canvas 画出内容并上屏。

- [x] 引入 Skia/V8 预编译依赖管线(tools/fetch_deps + deps.lock)——macOS arm64 已落地;Windows V8 来源待定(docs/09 §2.1)
- [x] runtime:JsRuntime(Isolate/Context)、ESM 模块加载、宿主类绑定基建(Wrappable/ClassBuilder)、Eval、console 绑定
- [x] graphics:DisplayListBuilder/DisplayList、LayerTree(Container/Offset/Transform/ClipRect/Opacity/Picture)
- [x] ui 绑定:Canvas/Paint/Gradient/Path/PictureRecorder/SceneBuilder/window.render
- [x] Rasterizer + SoftwareBackend(先软件,去 GPU 变量);Mac 软件 surface(CGImage → layer)
- [x] 验收:示例 `examples/hello_frame` 画渐变矩形与路径(Mac 已出帧, Windows 待 embedder/windows);golden 测试框架已建立(C++ 与 JS 共用基准)

## M2 · 帧管线与输入(约 3-4 周)

目标:动起来 + 点得到。

- [x] VsyncSource(Mac CVDisplayLink / Win DwmFlush 未验证);Animator;pipeline 背压
- [~] MetalBackend ✅(沙箱无 GPU, 真机待验证)+ 软件回退 ✅;AngleGlBackend ⬜(无 ANGLE 预编译包)
- [x] 输入:PointerData/KeyData 归一化(keycode/扫描码 → HID 全表)、事件打包投递(Float64Array 平铺复用);resize: Mac drawableSize 同步, Windows 同步等帧未做
- [x] MessageChannelHub + StandardCodec / StandardMethodCodec(JS/C++ 两端, Flutter 字节格式)
- [x] V8 Inspector(--dev / --inspect-port / --inspect-brk);M7 起改用 Node 的 inspector
- [x] 验收:`examples/bounce`(60fps + 鼠标跟随, 示例已于精简时删除);断点暂停/恢复经进程内会话测试, DevTools 连接需真机验证

## M3 · 文本与资源(约 3 周)

目标:能排版文字、显示图片。

- [x] text:FontCollection(系统字体 + 内嵌字体 + 回退)、ParagraphBuilder/Paragraph 绑定
- [x] assets:AssetManager、io 线程图片解码、Image/Codec 绑定;ImageCache 在框架层(LRU)
- [x] IME:Mac NSTextInputClient、Win IMM32(未验证)+ momo/textinput 通道(含替换区间)
- [~] 验收:`examples/text_images` ✅(示例已于精简时删除);文本测试为断言式(依赖系统字体, 未做像素 golden)

## M4 · Framework 层(约 6-8 周,与 M3 部分并行)

目标:用 View 框架写应用。

- [x] foundation/scheduler/painting(TextPainter)
- [x] rendering:RenderBox 协议、常用 render 对象(flex/stack/padding/text/image)、hit test、RepaintBoundary
- [x] views(原 widgets):Element diff、Stateful/Inherited、基础组件、runApp/ViewsBinding
- [x] gestures:arena、tap/drag/scroll;桌面 hover/光标/滚轮
- [x] animation;焦点与快捷键
- [x] headless 运行器(`momo_headless`, 框架测试在真实引擎 99 过/13 跳过)+ hot restart(--dev 监视 bundle)
- [x] 验收:counter 与 todo 全 View 实现;框架单测进 CTest(Node mock + 引擎 headless;CI 未建)

## M5 · 扩展机制(约 4 周)

目标:三方原生扩展端到端跑通。

- [x] extensions/sdk:momo_extension.h C ABI v1 定稿、momo_ext_sdk target
- [x] engine/extension:ExtensionHost、HostApi 实现(模块注册/通道/post_task/窗口句柄/帧回调/资源)
- [x] 纹理注入:TextureRegistry + TextureLayer + JS Texture widget(v1 CPU 像素帧)
- [x] manifest 生成与打包链路(`tools/momo-cli`)
- [x] 示例扩展:hello(纯 C)、texture;tray 示例已于精简时删除
- [~] 验收:`examples/extensions-demo` ✅(示例已于精简时删除);ABI 校验(`momo ext verify` + 纯 C 扩展)✅, 多编译器矩阵需 CI

## M6 · 打磨与发布(持续)

- [~] 冷启动:V8 code cache ✅(ESM 不支持完整 startup snapshot);首帧耗时未在真机测量
- [~] material 精简版(Theme/Button/Checkbox/TextField/Scaffold/Dialog);文本选择拖柄、撤销重做未做
- [~] 打包:.app + DMG(`momo build --dmg`)✅;MSIX、崩溃上报、性能浮层/Perfetto ⬜
- [x] 多窗口支持(Shell 多视图, macOS;docs/02 §2、docs/06 §2);Windows embedder 未实现
- [x] 分层整理: 窗口外观与行为、剪贴板/URL/输入法移出 engine, 改为 embedder 注册的宿主通道(`momo/window`、`momo/app`…,
      ARCHITECTURE §3.10);可选窗口能力显式声明(`WindowCapability`);所有窗口对等(视图 0 可关闭, `runView`);
      标题栏改为"应用声明高度、宿主报告控制按钮区域";拖动区域改为命中测试 + 手势竞技场;窗口跟随指针改为宿主移动循环;
      指针序列的接收方固定(框架 `capturePointer` 定逻辑接收方, 宿主定原生接收窗口, 引擎不参与);`PointerEvent.viewId`。Windows 只实现了定位与指针捕获两项可选能力(未编译验证)
- [x] 应用生命周期与窗口解耦: 应用状态只看应用本身(隐藏 = Suspended), 窗口可见性按视图(不可见的视图不出帧);
      最后一个窗口关闭不退出(`windowAllClosed`);退出流程 `Quit`(beforeQuit → 窗口 close → willQuit)/ `Exit(code)`;
      `Application` 与 `WindowManager` 拆分, 宿主通道统一为方法注册表(`HostChannel`);JS 侧 `app` / `Widget` 对象,
      组件基类改名为 `View`(对齐 Chromium views)。Windows 侧的窗口状态与 PlatformApp 未编译验证, `WM_ACTIVATEAPP` /
      `WM_QUERYENDSESSION` 尚未接入
- [ ] 无障碍(语义树 → UIA/AX)——设计预留,独立立项(未做)

## M7 · 接入 Node.js(参照 Electron `NodeBindings`)

完整嵌入 Node 26 LTS(libnode):JS 可用 `require`、`node:*`、npm 包与 Node 的定时器/IO。libuv 跑在 V8 所在的
ui 线程:`uv_run(UV_RUN_NOWAIT)` 由 ui 线程执行,独立的 poll 线程只等待 `uv_backend_fd`,两者以 `uv_sem` 轮流
(Electron `NodeBindings::UvRunOnce` / `EmbedThreadRunner`);入口 JS 经 `node::LoadEnvironment`;UI 事件仍直接调 V8,
外包 `node::CallbackScope`。

- [x] P0:libnode 预编译(`tools/build_libnode.py`,源码构建);momo 改用 libnode 内置的 V8 14.6;最低 macOS 13.5
- [x] P1:NodeBindings(uv 循环、poll 线程、`uv_sem`)、Node 平台替换 momo `V8Platform`、`node::Environment`、入口经
      Node ESM 加载器 + `uv_run` 泵送、UI 事件 `CallbackScope`、`momo:*` / `ext:*` 经 `module.registerHooks`、Node inspector
      (删除自研 inspector)、Node 编译缓存替换自研 code cache;文档 04/08、ARCHITECTURE
- [x] P2a:`process.exit()` 走应用的有序退出(`node::SetProcessExitHandler` → `Application::Exit`)
- [ ] P2:Windows `PollEvents`(IOCP,参照 Electron `node_bindings_win.cc`)与 libnode 的 Windows 构建;Node 的
      `AsyncLocalStorage` 上下文贯穿 momo 回调(`CallbackScope` 带 async context)

## 风险清单(提前盯防)

1. **Skia/V8 预编译供应链**:ICU 版本冲突、CRT 匹配——M1 第一周先打通,失败则降级为源码构建文档化路径。
2. **Windows GPU 兼容性**:坚持 ANGLE 路线,软件回退必须始终可用。
3. **JS↔C++ 边界性能**:M2 建立绘制压测基准(万级 draw op/帧),TypedArray 平铺方案不达标时改指令缓冲方案(JS 侧直接写 ArrayBuffer 指令流)。
4. **ABI 稳定性**:C ABI 一旦发布不可破坏——M5 前用两个真实扩展原型验证 API 形状再定稿。
