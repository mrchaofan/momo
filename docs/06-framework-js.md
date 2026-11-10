# 06 · Framework 层设计(TypeScript)

Framework 是 TS 编写、运行在引擎 V8 中的 UI 框架,对标 Flutter Framework:Widget/Element/RenderObject 三树、布局协议、绘制组织、手势、动画与调度。应用开发者只接触这一层 + 扩展包。

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
│   ├── widgets/       # Widget/Element 体系、StatefulWidget、基础组件(布局/滚动/文本/图片)
│   │                  #   binding.ts: runApp、WidgetsBinding(连接 scheduler/gestures/rendering)
│   ├── material/      # (后期)成品控件: Button/TextField/Dialog/Theme, 桌面交互规范
│   └── services/      # MessageChannel 封装、剪贴板、菜单、快捷键、生命周期
└── test/              # 基于 headless engine 的框架测试(golden + 逻辑)
```

模块依赖自下而上:`ui ← foundation ← scheduler/painting ← rendering/gestures/animation ← widgets ← material`。`services` 只依赖 foundation。

## 2. 三树模型

与 Flutter 相同的职责切分:

- **Widget**:不可变配置,`build()` 返回子描述;`Key` 控制身份。
- **Element**:持久化节点,持有 State,负责 diff(`canUpdate`: runtimeType+key)与子节点复用。TS 没有 Dart 的 runtimeType,以类静态 `kind` 符号 + 构造函数引用判等。
- **RenderObject**:布局/绘制/命中测试。`RenderBox` 实现盒模型:`layout(constraints, parentUsesSize)` → `performLayout()`;`markNeedsLayout/markNeedsPaint` 向上传播到 relayout/repaint boundary。

帧阶段(由 `WidgetsBinding` 挂到 `window.onBeginFrame/onDrawFrame`):

```
onBeginFrame: Ticker 推进动画
onDrawFrame : buildOwner.flushBuild → pipeline.flushLayout → flushCompositingBits
              → flushPaint(生成 Picture) → scene = 合成 Layer → window.render(scene)
```

### 多窗口(`widgets/view.ts`)

widget/element 树只有一棵,渲染树每个窗口一棵(对标 Flutter `View`/`ViewAnchor`):

- `ViewAnchor({ child, views })`:child 在原处正常渲染;views 挂在 element 树的这里(继承此处的 InheritedWidget 与状态),但渲染到各自的窗口。
- `Window({ title, width, height, onCloseRequested, onClosed, onOpened, child })`:挂载即 `createView` 打开窗口,卸载即关闭;用户关闭后回调 `onClosed`,应用据此从状态中移除。
- `View({ view, child })`:为窗口建立自己的 `RenderView` + `PipelineOwner`,向 `WidgetsBinding.registerView` 登记;每帧共用一次 build,各窗口分别 layout/paint/合成并 `view.render`。每个窗口有自己的 `MediaQuery`、`Overlay`(对话框、菜单只出现在本窗口)、默认快捷键与根焦点作用域(挂在焦点树根下,与主窗口并列;窗口获得系统焦点时恢复其上次的焦点)。
- 指针事件按来源窗口命中测试(`GestureBinding.handlePointerPacket(..., viewId)`),光标发往最近一次指针所在的窗口。
- 子树可以用 `GlobalKey` 在窗口之间移动并保留 State;含平台视图的子树移动时原生视图换窗口而不重建(MomoBrowser 的"移到新窗口")。

## 3. 布局与绘制协议

- 约束下行(`BoxConstraints`)、尺寸上行、父决定位置(offset 存在 parentData),单遍布局。
- 绘制:`paint(context, offset)`;`RepaintBoundary` 产生独立 Layer(对应 engine OffsetLayer,享受 raster cache)。框架 Layer 树是 engine LayerTree 的 TS 镜像,`SceneBuilder` 提交时一一映射。
- 文本:`TextPainter` 包装 `ui.ParagraphBuilder`,测量结果缓存按(文本,样式,宽度)键控。

## 4. 手势与桌面交互

- `onPointerPacket` → hit test(从 render 树根)→ `PointerRouter` 分发 → `GestureArena` 消歧(tap vs drag vs double-tap)。
- 桌面特性一等公民:hover(MouseRegion/光标形状)、精细滚轮与触控板惯性滚动、右键菜单、键盘焦点链(FocusNode/Traversal)、快捷键表(`Shortcuts`/`Actions` 模式)、文本选择。

## 5. 状态与更新

`setState` 标记 Element dirty → `scheduleFrame()`。首期不内置复杂状态管理,提供 `InheritedWidget` 等价物(依赖注册 + 精准重建)供生态在其上构建。

## 6. 与 JS 生态的关系

- 框架以 npm 包形式发布(`@momo/framework`),应用用 esbuild 打 bundle;**不依赖任何浏览器/Node API**,全局只存在引擎注入的 `momo:*` 模块——framework/ 的 lint 规则禁止引用 `window.document`、`fetch` 等。
- `services` 提供的网络/文件等能力由扩展包填充(如 `ext:fetch`),框架核心不内置 IO。

## 7. 测试

- 逻辑测试:headless engine(软件后端)+ 测试驱动的 `window`,可模拟 vsync 和指针序列(对标 flutter_test 的 WidgetTester)。
- Golden 测试:软件光栅化输出 PNG 比对,保证双端像素一致性回归。
