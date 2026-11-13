# 06 · Framework 层设计(TypeScript)

Framework 是 TS 编写、运行在引擎 V8 中的 UI 框架,对标 Flutter Framework:View/Element/RenderObject 三树(命名对齐 Chromium views: View = 组件, Widget = 原生窗口)、布局协议、绘制组织、手势、动画与调度。应用开发者只接触这一层 + 扩展包。

## 1. 包结构

```
framework/
├── package.json / tsconfig.json
├── packages/momo/src/
│   ├── ui/            # momo:ui 的 TS 类型声明与薄包装(Canvas/Scene/Paragraph/window)
│   ├── foundation/    # ChangeNotifier、诊断、断言、Key、平台常量
│   ├── scheduler/     # SchedulerBinding: 帧阶段、Ticker、任务优先级
│   ├── animation/     # AnimationController、Tween、Curve、Simulation(spring)
│   ├── gestures/      # 指针事件分发、GestureArena、tap/drag/scroll recognizer
│   ├── painting/      # TextPainter、Decoration、图片封装(ImageProvider/缓存)、边框圆角等值对象
│   ├── rendering/     # RenderObject/RenderBox 布局协议、命中测试、Layer 组织、常用 render 对象
│   ├── views/         # View/Element 体系、StatefulView、基础组件(布局/滚动/文本/图片)、RootView
│   │                  #   binding.ts: runApp / runView、ViewsBinding(连接 scheduler/gestures/rendering)
│   ├── material/      # (后期)成品控件: Button/TextField/Dialog/Theme, 桌面交互规范
│   └── services/      # MessageChannel 封装、剪贴板、菜单、快捷键、生命周期
└── test/              # 基于 headless engine 的框架测试(golden + 逻辑)
```

模块依赖自下而上:`ui ← foundation ← scheduler/painting ← rendering/gestures/animation ← widgets ← material`。`services` 只依赖 foundation。

## 2. 三树模型

与 Flutter 相同的职责切分:

- **View**:不可变配置,`build()` 返回子描述;`Key` 控制身份。
- **Element**:持久化节点,持有 State,负责 diff(`canUpdate`: runtimeType+key)与子节点复用。TS 没有 Dart 的 runtimeType,以类静态 `kind` 符号 + 构造函数引用判等。
- **RenderObject**:布局/绘制/命中测试。`RenderBox` 实现盒模型:`layout(constraints, parentUsesSize)` → `performLayout()`;`markNeedsLayout/markNeedsPaint` 向上传播到 relayout/repaint boundary。

帧阶段(由 `ViewsBinding` 挂到 `platformDispatcher.onBeginFrame/onDrawFrame`):

```
onBeginFrame: Ticker 推进动画
onDrawFrame : buildOwner.flushBuild → pipeline.flushLayout → flushCompositingBits
              → flushPaint(生成 Picture) → scene = 合成 Layer → viewport.render(scene)
```

### 应用与窗口(`services/app.ts`、`services/widget.ts`、`views/root_view.ts`)

view/element 树只有一棵,渲染树每个窗口一棵(对标 Flutter `View`/`ViewAnchor`、Chromium `views::Widget`/`RootView`):

- `app`:应用。`quit()`(经 `before-quit` / 每个窗口的 `close` / `will-quit`, 任一可取消)、`exit(code)`(不询问)、`hide` / `show` / `focus`;事件 `window-all-closed`(最后一个窗口关闭, 应用**不会**自动退出)、`activate`(点击 Dock 图标)、`did-become-active` / `did-resign-active`、`state-changed`(`launching` / `running` / `suspended` / `terminating`;suspended = 应用被隐藏, 不出帧)、`quit`。
- `Widget`:一个原生窗口。`Widget.create({ title, width, height, x, y, minWidth, minHeight, resizable, nonActivating, show })` 打开(宿主的 `momo/window`), `Widget.fromId(0)` 是启动窗口;方法 `close`(先触发可取消的 `close`)/ `destroy`、`show` / `hide` / `focus` / `minimize` / `maximize` / `restore`、`setTitle`、`setMinimumSize`、`setPosition`、`setResizable`、`setAlwaysOnTop`、`setTitleBar`、`startDrag`、`startMoveLoop`…;事件 `closed`、`focus` / `blur`、`show` / `hide`、`minimize` / `maximize` / `restore`、`move` / `resize`、`controls-changed`、`ready-to-show`(以 `show: false` 创建的窗口第一帧画好后)。窗口关闭后对象作废。
- 可取消事件是 `HostEvent`:`preventDefault()` 取消, `waitUntil(promise)` 异步决定。
- `RootView({ widget, child })`:把 child 渲染进窗口:为它建立自己的 `RenderView` + `PipelineOwner`,向 `ViewsBinding.registerView` 登记;每帧共用一次 build,各窗口分别 layout/paint/合成并 `viewport.render`(不可见的窗口跳过)。每个窗口有自己的 `MediaQuery`、`Overlay`(对话框、菜单只出现在本窗口)、默认快捷键与根焦点作用域(挂在焦点树根下,与其他窗口并列;窗口获得系统焦点时恢复其上次的焦点)。RootView 不打开也不关闭窗口。
- `ViewAnchor({ child, views })`:child 在原处正常渲染;views(各窗口的 RootView)挂在 element 树的这里(继承此处的 InheritedView 与状态),但渲染到各自的窗口。
- `WidgetScope.of(context)` 取子树所在的窗口;`Title({ title, child })` 声明窗口标题(`MaterialApp({ title })` 用它)。
- 外观由窗口内容里的 `WindowChrome({ hiddenTitleBar, titleBarHeight, resizeBorder })` 声明;隐藏标题栏后系统窗口控制按钮占用的区域由宿主报告, 经 `MediaQuery.windowControls` 提供(自绘标题栏据此避让, 不写死平台尺寸)。拖动窗口只有能力、没有框架内置区域: `Widget.startDrag(sequenceId)` 以进行中的按下序列(`PointerDownEvent.sequenceId`)开始系统拖动, 哪里可拖由应用决定(examples/webview 的 `WindowDragArea`)。
- 入口:`runApp(view)` 把根渲染进隐式视图 0;`runView(view)` 不绑定视图, 每个窗口(含启动窗口)的内容由 RootView 渲染(所有窗口对等)。任何窗口都可以关闭。
- 指针事件按来源窗口命中测试;每个 `PointerEvent` 带 `viewId`(同 Flutter), 光标等都按它找窗口, 没有"当前视图"这种全局状态。
- 子树可以用 `GlobalKey` 在窗口之间移动并保留 State;含平台视图的子树移动时原生视图换窗口而不重建(MomoBrowser 的"移到新窗口")。

## 3. 布局与绘制协议

- 约束下行(`BoxConstraints`)、尺寸上行、父决定位置(offset 存在 parentData),单遍布局。
- 绘制:`paint(context, offset)`;`RepaintBoundary` 产生独立 Layer(对应 engine OffsetLayer,享受 raster cache)。框架 Layer 树是 engine LayerTree 的 TS 镜像,`SceneBuilder` 提交时一一映射。
- 文本:`TextPainter` 包装 `ui.ParagraphBuilder`,测量结果缓存按(文本,样式,宽度)键控。

## 4. 手势与桌面交互

- `onPointerPacket` → hit test(从 render 树根)→ `PointerRouter` 分发 → `GestureArena` 消歧(tap vs drag vs double-tap)。
- 桌面特性一等公民:hover(MouseRegion/光标形状)、精细滚轮与触控板惯性滚动、右键菜单、键盘焦点链(FocusNode/Traversal)、快捷键表(`Shortcuts`/`Actions` 模式)、文本选择。
- 跨窗口拖拽:指针事件与拖动回调(`DragDown/Start/UpdateDetails`)带 `screenPosition`(逻辑点,主屏左上原点),
  与 `Widget.bounds`(内容区的屏幕框, 宿主推送更新)同一坐标系,不受窗口自身移动影响;
  `Widget.setPosition` 按内容区定位。窗口跟随指针用宿主的移动循环(`Widget.startMoveLoop(grabX, grabY)`):
  宿主在每个鼠标事件上同步移动窗口, 不经 ui 线程往返。
  指针序列的接收方是固定的: 按下期间 `MouseTracker` 冻结(不进入、不离开、光标不变), 松开后重新计算;
  `GestureBinding.capturePointer(route)` 显式固定接收方——之后这个指针的全部事件(不论来自哪个窗口)只交给 `route`,
  不经命中测试与竞技场, 原接收方收到取消(在拖动回调里捕获时推迟到当前事件分发完), 抬起或取消后结束;没有按下时返回 null。
  序列换窗口(拖出的新窗口打开、并入另一个窗口)用返回的 `PointerCapture.moveToView(viewId)`, 只改宿主里的原生接收窗口,
  触发拖动的窗口随后关闭也不中断(ARCHITECTURE §3.10)。示例见 examples/webview 的标签拖拽(会话独立于标签栏,同 Chromium
  `TabDragController`)。

## 5. 状态与更新

`setState` 标记 Element dirty → `scheduleFrame()`。首期不内置复杂状态管理,提供 `InheritedView`(依赖注册 + 精准重建)供生态在其上构建。

## 6. 与 JS 生态的关系

- 框架以 npm 包形式发布(`@momo/framework`),应用用 esbuild 打 bundle;**不依赖任何浏览器/Node API**,全局只存在引擎注入的 `momo:*` 模块——framework/ 的 lint 规则禁止引用 `window.document`、`fetch` 等。
- `services` 提供的网络/文件等能力由扩展包填充(如 `ext:fetch`),框架核心不内置 IO。

## 7. 测试

- 逻辑测试:headless engine(软件后端)+ 测试驱动的 `platformDispatcher`,可模拟 vsync 和指针序列(对标 flutter_test 的 WidgetTester;这里是 `ViewTester`)。
- Golden 测试:软件光栅化输出 PNG 比对,保证双端像素一致性回归。
