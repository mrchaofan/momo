# 05 · 渲染管线设计

定义 `engine/graphics/` 与 `shell/Rasterizer` 的设计:从 JS 绘制调用到屏幕像素的完整路径。

## 1. 管线总览

```
JS(ui线程)                                      raster 线程
Canvas.draw* ──记录──► DisplayList(不可变)
SceneBuilder.push*/addPicture ──组装──► LayerTree ──Animator投递──► Rasterizer
                                                     │ 1. Preroll(算脏区/缓存决策)
                                                     │ 2. AcquireFrame(RenderSurface)
                                                     │ 3. Paint: 遍历 Layer → 重放 DisplayList → SkCanvas
                                                     │ 4. flush(GrDirectContext) + Present
```

两级描述与 Flutter 相同:**DisplayList** 是"一段绘制指令"(帧内不可变、可重放、可跨线程共享);**LayerTree** 是"这一帧画面的合成结构"(变换/裁剪/透明度节点 + Picture 叶子)。

## 2. DisplayList

- `DisplayListBuilder` 实现与 `SkCanvas` 对齐的记录接口:save/restore/transform/clip、drawRect/RRect/Path/Image/Paragraph/Shadow、drawDisplayList(嵌套)。
- 指令存储:连续内存 arena 中的 POD 指令流(op code + 参数),重放时顺序 dispatch 到 `SkCanvas`,不直接使用 `SkPicture`(自持指令流便于做指令级优化、统计与未来替换后端)。
- 记录期做廉价优化:空 clip 剔除、冗余 save/restore 折叠、bounds 聚合(供脏区计算)。
- **M1 实现**(`engine/graphics/display_list.h`):指令 = POD `Op`(类型 + paint 索引 + 侧表索引 + 5 个参数),侧表存 Paint(连续相同的去重)、Path、Matrix、嵌套 DisplayList;bounds 按记录期矩阵与裁剪累计, 含无界绘制且无 cull rect 时为 nullopt。重放(`graphics/skia/skia_conversions.cc`)是 graphics 内部唯一接触 SkCanvas 的地方之一。
- `Paint` 为值类型(颜色、style、strokeWidth、shader/filter 引用),JS 侧字段平铺传入(docs/04 §3)。

## 3. LayerTree 与 Layer 类型

```
Layer(抽象: Preroll(ctx) / Paint(ctx))
├── ContainerLayer            # 子层列表
│   ├── TransformLayer        # 4x4 矩阵
│   ├── ClipRectLayer / ClipRRectLayer / ClipPathLayer
│   ├── OpacityLayer          # 可能触发 saveLayer; Preroll 决定是否可下推
│   ├── ImageFilterLayer / BackdropFilterLayer   # 模糊等(Backdrop 代价高, 标注)
│   └── OffsetLayer           # 纯平移, RepaintBoundary 对应物
├── PictureLayer              # 叶子: 持有 DisplayList + 偏移
├── TextureLayer              # 叶子: 外部纹理(扩展注入)
└── PlatformViewLayer         # 叶子: 平台视图(系统原生视图)占位, 合成时切片(§9)
```

已实现层: Container/Offset/Transform/ClipRect/ClipRRect/Opacity/Picture/Texture/PlatformView。OpacityLayer 的透明度继承已实现:
单子链且叶子不自重叠(单条绘制指令的 Picture、TextureLayer)时把 alpha 下推到绘制, 省掉 saveLayer。

**Preroll 阶段**计算:每层设备空间 bounds、`OffsetLayer` 的 raster cache 候选(重复帧未变化的子树缓存为纹理)、OpacityLayer 能否不开 saveLayer(单子且不相交时直接调制 alpha)。

## 4. GPU 后端

```cpp
// engine/shell/render_backend.h —— raster 线程创建与使用
class RenderBackend {
 public:
  virtual platform::SurfaceKind kind() const = 0;
  // 把 LayerTree 画进 surface 本帧给出的目标(不 present;GPU 后端返回前已提交命令)。
  virtual Status Draw(graphics::LayerTree& layer_tree, const platform::SurfaceFrame& frame) = 0;
};
// 按 surface.kind() 选择: kMetal → MetalBackend, kSoftware → SoftwareBackend, kNone → NullBackend。
StatusOr<std::unique_ptr<RenderBackend>> CreateRenderBackend(platform::RenderSurface& surface);
```

| 后端 | 平台 | 说明 |
|---|---|---|
| `MetalBackend` | macOS | CAMetalLayer + GrMtlBackendContext,首选 |
| `AngleGlBackend` | Windows | ANGLE(EGL→D3D11) + GrGLInterface;与 Chromium/Flutter Windows 同路线,避开驱动坑 |
| `SoftwareBackend` | 双端 | SkSurface raster + 平台位图呈现(Win: StretchDIBits;Mac: layer contents),远程桌面/无 GPU 回退。**M1 已实现**:`graphics::RasterizeLayerTree` 画进 `platform::SoftwarePixels`(BGRA 预乘),Mac `MacSoftwareSurface`:raster 线程直接画进 IOSurface(≤3 个缓冲, 零拷贝, sRGB 标记由 WindowServer 做色彩匹配), 主线程把它设为 layer.contents;**主线程最多挂起一帧**(新帧覆盖待显示槽, 槽从空变满才投递), 缓冲全忙时丢帧——保证 raster 快于主线程时主队列与内存有界、输入不被饿死 |

后端选择在 Rasterizer 初始化时探测,失败逐级降回软件。**M2 已实现**: `MetalBackend`(`engine/graphics/gpu/metal_rasterizer.mm`,
CAMetalLayer drawable 包装为 render target)与回退链(GPU 后端创建失败 → `RenderSurface::FallBackToSoftware()` → 软件后端);
`AngleGlBackend` 未实现(Windows)。io 线程可建共享 context 做纹理预上传(M5+ 优化项,首期图片在 raster 线程惰性上传)。

## 5. 帧调度与同步

- `Animator`(ui 线程):`scheduleFrame()` → 向 VsyncSource 请求回调 → vsync 到来时调 JS `onBeginFrame/onDrawFrame` → 收到 LayerTree 后入渲染队列。
- **背压**:渲染队列深度 1 + 正在绘制 1;若 raster 落后,Animator 跳过 vsync 不再驱动 JS(丢 build 不丢最新画面),与 Flutter pipeline depth 策略一致。
- **resize**:platform 线程收到尺寸变化 → metrics 投递 ui 线程触发新帧;raster 线程在 AcquireFrame 时以 surface 当前尺寸为准。
  **拖拽缩放(live resize)同步**(macOS,`MacMetalSurface::WaitForFrame`,同 Flutter `FlutterResizeSynchronizer`):窗口尺寸的变化
  在主线程的 CATransaction 里提交,而 raster 线程异步 present 的帧与之不同步——窗口服务器拿到新尺寸时内容仍是旧帧(被
  `contentsGravity` 拉伸),直到松手。因此 live resize 期间全部帧改经**主线程呈现队列**(`presentsWithTransaction`);内容视图每次
  `setFrameSize:` 后先把新尺寸交给引擎,再在主线程等待一帧该尺寸的帧(连同之前排队的帧按序)在当前事务里呈现——新尺寸与新内容、
  平台视图几何同一事务生效。等待期间以 `EventLoop::RunDueTasksNested` 执行平台线程任务(vsync 请求与回调要经过主线程,否则死锁);
  上限由应用配置(`view.setResizeOptions({ liveResizeFrameWait })` / `WindowChrome.liveResizeFrameWait`,缺省 100 ms,0 = 不等待),
  应用更慢时退化为暂时拉伸,不让窗口拖动卡住。软件 surface 尚未同步。Windows 上活动 resize 期间 platform 线程同样
  阻塞等待该帧 present(消除拉伸)。
- 首帧特殊路径:`Rasterizer` 支持 `DrawLastLayerTree` 以便窗口显示前预热 shader/上下文。

## 6. 文本与图片在管线中的位置

- `Paragraph`(SkParagraph)在 ui 线程 layout(布局需要同步测量),绘制记录为 DisplayList 的 drawParagraph 指令,raster 线程执行真正的 glyph 光栅化(Skia 内部 glyph cache 跨帧复用)。
- `Image` 解码产物为 `SkImage`(io 线程解码为位图,raster 线程首次使用时上传 GPU);DisplayList 持有其 shared_ptr,确保跨线程存活。

## 7. Golden 测试

`engine/graphics/testing/golden.h`:软件光栅结果与 `engine/graphics/testdata/*.png` 比对(每通道 ±2 容差,失败时把实际结果写到临时目录);`MOMO_UPDATE_GOLDENS=1` 重新生成。M1 验收画面 `acceptance_scene` 同时由 C++(graphics_unittests)和 JS(ui_unittests,经 momo:ui)生成并比对同一张基准,保证 JS 绑定到光栅的整条链路一致。

## 8. 诊断

帧时间轴埋点(build/layout/paint/raster 各阶段耗时)经 `momo/diagnostics` 通道暴露;`--dev` 下 JS 可开启性能浮层(由框架层绘制)。可选集成 Perfetto SDK(M6)。

## 9. 平台视图(platform view)

把扩展创建的**系统原生视图**(macOS `NSView`,如 `WKWebView`)嵌进 momo 画面,与 momo 内容按绘制顺序交替叠放。
思路与 Flutter 的 hybrid composition 相同:原生视图仍由系统合成,momo 把自己的画面切成多层透明图层夹住它。

```
JS(ui)                         raster                                    platform(主线程)
PlatformView widget            Composition::Build(LayerTree)             MomoPlatformViews(embedder)
 └ SceneBuilder.addPlatformView  1. Preroll 收集平台视图(变换/裁剪/透明度)
   → PlatformViewLayer           2. N 路画布分片录制: slice0 = 底层,
                                    slice i = views[i-1] 之上的内容
                                 3. AcquireCompositionFrame(布局)
                                 4. 分片画进主帧 / 各 overlay 帧
                                 5. present ───────────────────────────► 同一 CATransaction:
                                                                          视图几何 + 全部 drawable 呈现
```

- **契约**:`engine/platform/platform_view.h`(`CompositionLayout`:每个视图的设备空间 frame、祖先裁剪、透明度、
  遮挡矩形;每个 overlay 的内容范围),`RenderSurface::SupportsPlatformViews / AcquireCompositionFrame`(可选能力)。
- **分片**(`engine/graphics/composition.{h,cc}`):容器层的变换/裁剪/saveLayer 经 `SkNWayCanvas` 同步到全部分片,
  叶子(Picture/Texture)只画进当前分片;遇到 `PlatformViewLayer` 切到下一个分片。每个分片记录叶子的设备空间范围,
  用于 overlay 图层尺寸(只覆盖有内容的区域)和**命中遮挡**(视图之上有 momo 内容的区域,点击归 momo)。
- **Rasterizer**:层树含平台视图(`LayerTree::has_platform_views`,由 SceneBuilder 标记)且 surface 支持时走合成路径;
  视图消失后再走一帧"空布局"让宿主隐藏视图,随后回到普通路径。surface 不支持时视图不显示、其余照常(只告警一次)。
- **macOS**(`embedder/macos/mac_platform_views.mm`、`mac_surface_metal.mm`):内容视图上挂一个铺满的宿主视图,
  每个平台视图一个容器(可见区域 + 圆角裁剪 + alpha),每个 overlay 一个承载 CAMetalLayer 的视图;z 序用 layer
  `zPosition` 表达(不移除/重排子视图, 以免原生视图丢失第一响应者)。合成模式下所有 CAMetalLayer
  `presentsWithTransaction = YES`,raster 等命令被 GPU 调度后,把"几何 + present"投递到主线程同一事务,
  使原生视图与 momo 画面同帧生效;退出合成时,若主线程仍有待呈现帧,普通帧也经主线程呈现以保证顺序。
- **输入与焦点**:**谁接收指针事件由框架决定**,embedder 只提供投递能力(同 Chromium Aura:`aura::Window` 不是系统窗口,
  `CaptureClient::SetCapture` 的参数是它,`RenderWidgetHostViewAura` 负责把事件交给渲染器)。
  ```
  NSEvent ─► MomoWindow -sendEvent: ─► MomoContentView: 指针包 + native_event_id(原始 NSEvent 暂存于环形缓冲)
                                          │ ui 线程
                                          ▼
             GestureBinding 命中测试 / 按下序列沿按下时的命中路径(隐式捕获)/ 触控板手势锁定(scroll_phase)
                                          │ 落到 RenderPlatformView
                                          ▼
             view.dispatchPlatformViewPointer(platformViewId, nativeEventId, action) ─► Shell ─► PlatformWindow
                                          │ platform 线程
                                          ▼
             按 id 取回原始 NSEvent ─► MomoPlatformViewContainer(原生视图的宿主)直接调用原生视图的响应者方法
  ```
  - **framework**(`rendering/platform_view.ts`、`gestures/binding.ts`):`RenderPlatformView` 参与命中测试,命中路径上的
    按下/移动/抬起/悬停经 `GestureBinding.forwardToPlatformView` 回送;滚动先于外层 Scrollable 在 `PointerSignalResolver`
    登记(滚动只归原生视图);作为 `MouseTrackerAnnotation`,进入/离开即指针归属(`setPlatformViewHover`),光标 `defer`。
    盖在平台视图上且可命中的 momo 内容自然先命中——命中测试即遮挡判断。原生按下若没有任何平台视图接收,
    回送"平台视图 0"(由 momo 消费),宿主据此把键盘焦点交还内容视图。
  - **全部位置型输入走同一条路线**(同 Chromium Aura 与 Flutter Android:原生 → 框架 → 原生):鼠标按钮/移动/滚动之外,
    触控板捏合/旋转/轻扫/智能缩放、查询(Quick Look)、Force Touch 压力、手写板点、手势开始/结束也先交给框架
    (`kGesture` 指针事件 → `PointerGestureEvent` / `PointerPressureEvent`),由框架命中测试(触控板手势按阶段锁定;
    压力沿按下时的命中路径)后再回送给原生视图。宿主不再让 AppKit 按几何把任何位置型输入直接投递给原生视图;
    仍直接交给 AppKit 的只有窗口装饰(红绿灯、拖动区域)的按下序列与非位置型事件(键盘等)。
    原始事件不经 AppKit 投递给任何视图;原生视图只经框架回送的事件(按框架处理的顺序)收到输入。
  - **engine**:`PointerData` 增加 `native_event_id`(窗口内递增,< 2^53)、`signal_phase`(none/start/update/end)、
    `gesture_kind` / `gesture_value` / `gesture_stage`;指针包步长 17;`momo:ui View.dispatchPlatformViewPointer` → `Shell::Delegate::OnEnginePlatformViewPointer`
    → `PlatformWindow::DispatchPlatformViewPointer`(异步投递到 platform 线程)。
  - **macOS embedder**:分三块,职责与 Chromium / Flutter 对应——
    | 组件 | 职责 | 对应 |
    |---|---|---|
    | `MomoWindow -sendEvent:`(`mac_window_body.inc`)+ `MomoContentView` | 原始输入只交给 momo(指针包 + 原生事件 id),不经 AppKit 投递;点击激活自己完成 | aura `WindowTreeHost` |
    | `MomoPlatformViewInputRouter`(`mac_platform_view_input.{h,mm}`,窗口级,非视图) | 原生事件环形缓冲;按框架的回送投递给原生视图;first mouse;跟踪区进出的推导;指针归属 | `RenderWidgetHostInputEventRouter` / aura `WindowEventDispatcher` |
    | `MomoPlatformViews` 的容器(`mac_platform_views.mm`) | 只管几何:裁剪、圆角、透明度、z 序;回答 AppKit 的几何命中 | Flutter `FlutterMutatorView` |

    原始的鼠标与位置型输入只交给内容视图,**不经 AppKit 投递**;只有窗口装饰(红绿灯、拖动区域)的按下序列与它们
    自己的跟踪区事件直接交给 AppKit。**系统的边缘缩放区**(可缩放、非全屏窗口内侧的缩放边框,`inSystemResizeZone:`)优先于拖动区域与
    momo 内容:光标在这里由系统显示为缩放形状,左键按下也先交给 AppKit(它在 `-[NSWindow sendEvent:]` 内跑缩放的跟踪循环);
    松开后按键已不再按下即说明 AppKit 接管了整个序列,否则照常交给拖动区域或 momo。试探期间命中测试不落到任何平台视图、第一
    响应者保持不变——光标与点击按同一个判断,看到缩放光标就能缩放(隐藏标题栏的窗口顶部即拖动区域的上边缘)。
    **缩放行为由应用配置**(`view.setResizeOptions` / 框架的 `WindowChrome`,`engine/platform/view.h` 的 `WindowResizeOptions`):
    `resizable`(缺省 true,`createView` / `Window` 也可在打开时给出)、`border`(内侧缩放边框,逻辑点,四边或分边,缺省 5;
    0 = 该边没有内侧边框,应用内容收到全部输入,系统在窗口外侧的缩放区仍可用)、`liveResizeFrameWait`(见 §4 resize)。
    未给出的字段取缺省值;框架的 `WindowChrome` 不给任何缩放项时不改动窗口。内容视图不在按下时抢第一响应者(否则点击已聚焦的网页会先失焦再聚焦),
    按下由 momo 消费时由框架回送"平台视图 0"再取回。零位移的滚动阶段事件也上报(原生视图需要完整手势)。
    框架回送后,输入路由按事件类型交给原生视图:
    - 按钮/拖拽/抬起、滚动、触控板手势、压力、手写板点:经 AppKit 的正常投递(`MomoMouseRouting
      momoDeliverEvent:toPlatformView:` 直接调用 `-[NSWindow sendEvent:]` 的父类实现),投递期间宿主的命中测试只落在
      该平台视图的原生视图子树里。AppKit 完成其余的一切:按下视图的捕获(拖拽/抬起/压力发给收到按下的视图)、
      第一响应者、`NSGestureRecognizer`(状态机、`delaysPrimaryMouseButtonEvents`、互斥;WKWebView 用力点按预览靠
      `NSImmediateActionGestureRecognizer`)、滚动与手势锁定。哪个平台视图接收由框架决定,AppKit 只在该原生视图内部
      按它自己的规则投递。
    - first mouse:事件到达时窗口不是 key(窗口已在原始事件上被激活),由被点到的视图的 `acceptsFirstMouse:`
      决定这次点击是否只用于激活窗口——不接受时整个按下序列都不投递。
    - 移动:不经 AppKit(AppKit 把移动发给 first responder,未聚焦的原生视图收不到),按 AppKit 的跟踪区规则投递给
      原生视图子树中包含指针、当前生效的 `NSTrackingMouseMoved` 跟踪区的 owner(WKWebView 不实现 `mouseMoved:`,
      悬停全靠其跟踪区 owner `WKMouseTrackingObserver`);没有这样的跟踪区时交给命中视图。
    - **AppKit 的几何命中**(不经框架投递时"指针下是哪个视图"的查询:光标归属、拖放目标):momo 掌握指针位置时
      以框架的指针归属为准——指针不归某个平台视图,它的容器就不可命中。**光标因此只有一个裁决者**:WebKit 在
      `[NSCursor set]` 之前用内容视图的 `hitTest:` 确认指针下是它自己的子树(`PageClientImpl::setCursor`,注释说明
      用于防止迟到的 IPC 覆盖兄弟视图的光标),指针离开或被 momo 内容盖住后,网页迟到的光标设置被它自己丢弃。
      拖放会话等期间框架收不到鼠标事件,退回按画面上的遮挡(occluders)近似。
    环形缓冲 128 项,溢出的事件不再投递(VERBOSE 日志)。
  - **指针归属与跟踪区**:原生视图的跟踪区状态(进入/离开/光标更新)只由一条**有序输入流**推导——框架回送的事件与
    框架的指针归属(`MouseTracker` 进入/离开,带触发它的原生事件 id),二者经同一通道按框架处理的顺序到达。
    框架先更新悬停再分发(同 Flutter `RendererBinding.dispatchEvent`),原生视图因此先"进入"再收到该位置的事件。
    容器在流中的每个事件上按 AppKit 的规则重算子树中各跟踪区:指针归本视图、位于跟踪区内、跟踪区生效 → 在内;
    先离开后进入,进入时带 `NSTrackingCursorUpdate` 的补一次光标更新;拖动期间不带 `NSTrackingEnabledDuringMouseDrag`
    的跟踪区推迟到抬起;已移除的跟踪区不再投递。失去归属 = 全部离开(在触发事件的位置,如窗口外)。
    **AppKit 为这些跟踪区生成的事件一律不投递**:它们同步产生,而转发的移动经 ui 线程往返,两者没有先后关系——
    指针从网页直接移出窗口时,AppKit 的离开先到、之前的移动后到,原生视图会在离开之后又被带回悬停状态。
    同 Chromium:渲染器看到的 MouseLeave 与移动在同一个有序输入队列里(`RenderWidgetHostViewCocoa` 把
    `mouseExited` 转为输入事件,而不是让渲染器自己观察系统的进出)。
    其他跟踪区:momo 内容视图的交给内容视图(离开窗口 → 指针 remove,带离开事件的 id);窗口装饰(主题框架、标题栏、
    红绿灯)的交给 AppKit,未交给 AppKit 的移动也转给装饰自己的跟踪区,装饰据此维护窗口级状态(边缘缩放光标)。
    光标归属:装饰在处理中设置了光标(缩放区)则归装饰;指针归平台视图时归原生视图(momo 不设置);否则 momo 的光标
    (原生视图迟到的设置被它自己的命中检查丢弃,见上)。
  - 嵌套事件循环(原生右键菜单)吞掉抬起时,下一次移动/按下发现按键已松开,补发抬起复位框架侧状态。
  - 焦点:原生视图获得焦点时经 `momo/platform_views` 通道发 `focused`,框架把 momo 焦点移到对应 `PlatformView`。
  - 代价与限制:原生视图的输入多一次 platform → ui → platform 的往返(ui 线程繁忙时网页输入随之延迟);重放的是原始
    `NSEvent`,但已不在其同步处理中(`NSApp.currentEvent` 可能是更新的事件)——WKWebView 本身异步处理输入,不受影响;
    在 `mouseDown:` 里跑嵌套跟踪循环的普通 `NSView`(如 `NSButton`)可能等不到已被处理的抬起。跨窗口捕获
    (`view.setCapture()`)期间的事件不重放给平台视图。经 AppKit 重放的是原始 `NSEvent`,但已不在其
    同步处理中:`[NSEvent pressedMouseButtons]` / `mouseLocation` 反映的是当前状态而不是事件发生时。框架侧尚无手势竞技场参与(外层手势识别器与原生视图同时收到事件)。
- **生命周期**:视图由 JS 经 `momo/platform_views`(create/dispose/focus)请求,Shell 在 platform 线程调用扩展工厂
  (`PlatformViewRegistry`);热重启与关闭时引擎销毁全部视图(先移出窗口再交给扩展释放,早于扩展 `on_detach`)。
  JS 侧视图 id 带每个运行时随机的高位标签,避免热重启前的在途消息误伤新视图。
- **限制**:只支持平移与正向缩放(旋转/斜切按包围盒近似并告警);圆角只支持与可见区域贴合的一层;
  软件渲染 surface 不支持平台视图;Windows embedder 未实现(契约已就绪)。
