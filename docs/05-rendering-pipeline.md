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
| `Direct3DBackend` | Windows | DXGI flip-model 交换链 + Ganesh D3D12(`GrD3DBackendContext`;skia-pack 的 Windows 包自带);未在 Windows 上运行验证 |
| `SoftwareBackend` | 双端 | SkSurface raster + 平台位图呈现(Win: StretchDIBits;Mac: layer contents),远程桌面/无 GPU 回退。**M1 已实现**:`graphics::RasterizeLayerTree` 画进 `platform::SoftwarePixels`(BGRA 预乘),Mac `MacSoftwareSurface`:raster 线程直接画进 IOSurface(≤3 个缓冲, 零拷贝, sRGB 标记由 WindowServer 做色彩匹配), 主线程把它设为 layer.contents;**主线程最多挂起一帧**(新帧覆盖待显示槽, 槽从空变满才投递), 缓冲全忙时丢帧——保证 raster 快于主线程时主队列与内存有界、输入不被饿死 |

后端选择在 Rasterizer 初始化时探测,失败逐级降回软件。**M2 已实现**: `MetalBackend`(`engine/graphics/gpu/metal_rasterizer.mm`,
CAMetalLayer drawable 包装为 render target)与回退链(GPU 后端创建失败 → `RenderSurface::FallBackToSoftware()` → 软件后端);
`Direct3DBackend`(`engine/graphics/gpu/d3d_rasterizer.cc`, 后台缓冲包装为 render target;每帧 `flush(kPresent)` 后等待 GPU 完成,
让 Skia 释放对后台缓冲的引用, surface 才能随时 `ResizeBuffers`)。surface 运行中自行改变种类时(Windows 设备丢失后
`WinD3DSurface` 回退软件), Rasterizer 在下一帧发现 `surface.kind()` 与后端不一致并重建后端;重建 GPU 设备未实现。io 线程可建共享 context 做纹理预上传(M5+ 优化项,首期图片在 raster 线程惰性上传)。

## 5. 帧调度与同步

- `Animator`(ui 线程):`scheduleFrame()` → 向 VsyncSource 请求回调 → vsync 到来时调 JS `onBeginFrame/onDrawFrame` → 收到 LayerTree 后入渲染队列。
- **背压**:渲染队列深度 1 + 正在绘制 1;若 raster 落后,Animator 跳过 vsync 不再驱动 JS(丢 build 不丢最新画面),与 Flutter pipeline depth 策略一致。
- **resize**:platform 线程收到尺寸变化 → metrics 投递 ui 线程触发新帧;raster 线程在 AcquireFrame 时以 surface 当前尺寸为准。
  **拖拽缩放(live resize)同步**(macOS,`MacMetalSurface::WaitForFrame`,同 Flutter `FlutterResizeSynchronizer`):窗口尺寸的变化
  在主线程的 CATransaction 里提交,而 raster 线程异步 present 的帧与之不同步——窗口服务器拿到新尺寸时内容仍是旧帧(被
  `contentsGravity` 拉伸),直到松手。因此 live resize 期间全部帧改经**主线程呈现队列**(`presentsWithTransaction`);内容视图每次
  `setFrameSize:` 后先把新尺寸交给引擎,再在主线程等待一帧该尺寸的帧(连同之前排队的帧按序)在当前事务里呈现——新尺寸与新内容、
  平台视图几何同一事务生效。等待期间以 `EventLoop::RunDueTasksNested` 执行平台线程任务(vsync 请求与回调要经过主线程,否则死锁);
  上限 100 ms(宿主的实现细节, 不对应用暴露;同 Flutter 的 resize synchronizer),
  应用更慢时退化为暂时拉伸,不让窗口拖动卡住。macOS 软件 surface 尚未同步。Windows(`WinWindow::SynchronizeLiveResize`):
  系统缩放循环里的 `WM_SIZE` 先通知新尺寸, 再等待 D3D 交换链(或软件 surface)呈现一帧该尺寸的帧, 同样上限 100 ms。
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
  叶子(Picture/Texture)只画进当前分片;遇到 `PlatformViewLayer` 切到下一个分片。每个分片记录叶子的设备空间范围
  (overlay 图层尺寸, 只覆盖有内容的区域)与**可命中区域**(Picture 的 `hit_regions`: 框架绘制时经
  `Canvas.addHitRegion` 记录的包围盒, 换算到设备空间并按裁剪截断)——视图之上的可命中区域即**命中遮挡**(点击归 momo)。
- **Rasterizer**:层树含平台视图(`LayerTree::has_platform_views`,由 SceneBuilder 标记)且 surface 支持时走合成路径;
  视图消失后再走一帧"空布局"让宿主隐藏视图,随后回到普通路径。surface 不支持时视图不显示、其余照常(只告警一次)。
- **macOS**(`embedder/macos/mac_platform_views.mm`、`mac_surface_metal.mm`):内容视图上挂一个铺满的宿主视图,
  每个平台视图一个容器(可见区域 + 圆角裁剪 + alpha),每个 overlay 一个承载 CAMetalLayer 的视图;z 序只用子视图顺序表达, 变化时
  `sortSubviews` 原地重排(不移除/重新加入子视图, 以免原生视图丢失第一响应者)。合成模式下所有 CAMetalLayer
  `presentsWithTransaction = YES`,raster 等命令被 GPU 调度后,把"几何 + present"投递到主线程同一事务,
  使原生视图与 momo 画面同帧生效;退出合成时,若主线程仍有待呈现帧,普通帧也经主线程呈现以保证顺序。
- **输入与焦点**:**原生事件的接收方由宿主在 platform(主)线程按每一帧的几何同步决定**,不经 ui 线程往返
  (同 Chromium 合成器线程用已提交帧的命中测试数据处理输入、Flutter iOS 用已光栅化帧的 overlay 几何做命中)。
  ```
  raster 每帧: Composition → CompositionLayout{视图可见矩形、裁剪、z 序、盖在其上的 momo 可命中区域 occluders}
       │ 与 drawable 一起投递到主线程, 同一个 CATransaction 里 applyLayout + present(几何 = 用户看到的画面)
       ▼
  NSEvent ─► MomoWindow -sendEvent: ─► MacWindow::SendEvent ─► MacMouseInput 翻译为 PointerInput ─► PointerDispatcher(embedder/common, 进程内一个, MacMouseInput 持有)
       HitTest: 本应用最上层的窗口 → 内容区 → platformViewAtWindowPoint:(在可见区域内且不在 occluders 里)
         ├─ 平台视图 N ─► MacWindow::SendToPlatformView ─► MomoPlatformViewInputRouter ─► 经 AppKit 投递(momo 不收到)
         └─ 0(momo 内容)─► MacWindow::SendToMomo ─► 指针包 ─► ui 线程(框架照常命中测试、手势、悬停)
  ```
  - **派发器**(`PointerDispatcher`,中介者):接收方的全部状态(按下序列、悬停目标、滚动/手势锁定、momo 是否已接入指针)
    只在这里有一份;平台层只翻译原生事件、执行投递。规则见 ARCHITECTURE §3.10"指针派发",单测覆盖每条规则。
  - **按下**决定这个序列的接收方并固定到抬起(拖动中几何变化、被 momo 内容盖住、拖到别的窗口都不换);momo 的序列(包括
    框架 `capturePointer` 的拖动会话)经过网页时网页什么也收不到。**滚动与触控板手势**在开始(或没有阶段的单个事件)时
    锁定接收方到结束;压力、按下中的手写板点跟随按下序列。
  - **悬停**:没有按下时指针下的接收方即悬停目标;变化时同步推导原生视图的进入/离开/光标归属,指针进入平台视图时
    momo 收到 remove(只表示离开),回到 momo 内容时重新 add。画面变化而指针不动(菜单弹出盖住网页、网页出现在指针下)时,
    几何生效后的下一轮主循环重新判定(`geometryChanged` → `PointerDispatcher::GeometryChanged`)。
  - **可命中区域**:框架绘制时,自身可命中的渲染对象(`hitTestSelf` 为真)记录自己的包围盒,`IgnorePointer` 子树与平台视图
    本身不记录;只绘制、不可交互的内容(阴影、拖动中的浮动副本)因此不挡网页的点击。
  - **焦点**:按下由 momo 接收且第一响应者在平台视图里时,窗口取回第一响应者;按在平台视图上由 AppKit 让原生视图
    成为第一响应者(不先取回,否则已聚焦的网页会失焦再聚焦)。原生视图获得焦点时经 `momo/platform_views` 发 `focused`,
    框架把 momo 焦点移到对应 `PlatformView`。
  - 框架里的 `RenderPlatformView` 只在命中测试中占位:几何刚变化、新帧尚未呈现时落到这里的事件不穿透到下面的 momo 内容。
  - **投递**(`MomoPlatformViewInputRouter`,只执行不决定):按钮/拖拽/抬起、滚动、触控板手势、压力经 AppKit 的正常投递
    (`MomoMouseRouting momoDeliverEvent:toPlatformView:`,投递期间宿主的命中测试只落在该原生视图的子树里),按下视图的
    捕获、第一响应者、手势识别器、immediate action 由 AppKit 完成;first mouse 按被点到的视图的 `acceptsFirstMouse:`。
    移动按 AppKit 的跟踪区规则投递给 `NSTrackingMouseMoved` 跟踪区的 owner(WKWebView 的悬停靠 `WKMouseTrackingObserver`)。
    原生视图跟踪区的进入/离开/光标更新由投递的事件与悬停目标按 AppKit 的规则推导(拖动期间不带
    `NSTrackingEnabledDuringMouseDrag` 的跟踪区推迟到抬起);AppKit 自己为这些跟踪区生成的事件不投递。
    AppKit 的几何命中(光标归属、拖放目标)以派发器的判定为准(`MomoPlatformViews.pointerOwner`):指针不归某个平台视图,
    它的容器就不可命中——WebKit 在 `[NSCursor set]` 之前用 `hitTest:` 确认指针下是自己,光标因此只有一个裁决者。
  - **竞态**:判定依据已呈现的帧,即用户此刻看到的画面(JS 刚打开、尚未呈现的菜单还不挡点击——用户也还没看到它);
    派发器与几何都只在主线程读写,没有跨线程的竞争。嵌套事件循环(原生右键菜单)吞掉抬起时,下一次无按键的移动或
    新的按下结束旧序列。
  - 限制:可命中区域按包围盒记录(圆形按钮的四角也算);框架侧的手势识别器不与原生视图竞争(落在网页上的事件 momo 看不到)。
- **生命周期**:视图由 JS 经 `momo/platform_views`(create/dispose/focus)请求,Shell 在 platform 线程调用扩展工厂
  (`PlatformViewRegistry`);热重启与关闭时引擎销毁全部视图(先移出窗口再交给扩展释放,早于扩展 `on_detach`)。
  JS 侧视图 id 带每个运行时随机的高位标签,避免热重启前的在途消息误伤新视图。
- **限制**:只支持平移与正向缩放(旋转/斜切按包围盒近似并告警);圆角只支持与可见区域贴合的一层;
  软件渲染 surface 不支持平台视图;Windows embedder 未实现(契约已就绪, 需要子 HWND + 覆盖层合成)。
