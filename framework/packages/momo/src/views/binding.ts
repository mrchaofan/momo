import { SceneBuilder, viewports, platformDispatcher, type KeyData, type Viewport } from "momo:ui";
import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { GestureBinding } from "../gestures/binding";
import { Rect, Size } from "../painting/geometry";
import { MouseTracker } from "../rendering/mouse_tracker";
import { PipelineOwner } from "../rendering/object";
import { RenderView, type ViewConfiguration } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import { app, type AppState } from "../services/app";
import { TextInput } from "../services/text_input";
import { Widget } from "../services/widget";
import { Actions, Shortcuts, defaultActions, defaultShortcuts } from "./actions";
import { FocusManager, FocusScopeNode } from "./focus_manager";
import { FocusScope } from "./focus_scope";
import {
  BuildOwner, Element, RenderObjectElement, RenderObjectView, State, StatefulView, type BuildContext, type View,
} from "./framework";
import { MediaQuery, MediaQueryData } from "./media_query";
import { Overlay, OverlayEntry } from "./overlay";
import { WidgetScope } from "./widget_scope";
import type { RenderObject } from "../rendering/object";
import type { RenderBox } from "../rendering/box";

/**
 * 渲染侧的观察者: 隐式视图(runApp 渲染的窗口)的尺寸变化。应用与窗口的事件(退出、关闭、焦点、状态)在
 * app 与 Widget 对象上订阅。
 */
export interface ViewsBindingObserver {
  didChangeMetrics?(): void;
}

/** 隐式视图 id(宿主启动时创建的窗口)。 */
export const kImplicitViewId = 0;

/** 一个窗口在渲染侧的状态(由 RootView 注册;隐式视图由 runApp 的根渲染, 不在这里)。 */
export interface ViewRenderHost {
  readonly viewport: Viewport;
  readonly renderView: RenderView;
  readonly pipelineOwner: PipelineOwner;
}

/** 根 view:把子树挂到 RenderView。 */
class BindingRootView extends RenderObjectView {
  constructor(readonly child: View | null, readonly renderView: RenderView) {
    super();
  }
  createElement(): Element {
    return new RootElement(this);
  }
  createRenderObject(): RenderObject {
    return this.renderView;
  }
}

class RootElement extends RenderObjectElement {
  #child: Element | null = null;
  override visitChildren(visitor: (child: Element) => void): void {
    if (this.#child !== null) visitor(this.#child);
  }
  override forgetChild(child: Element): void {
    if (child === this.#child) this.#child = null;
  }
  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.#child = this.updateChild(this.#child, (this.view as BindingRootView).child, null);
  }
  override update(newView: View): void {
    super.update(newView);
    this.#child = this.updateChild(this.#child, (this.view as BindingRootView).child, null);
  }
  insertRenderObjectChild(child: RenderObject): void {
    (this.renderObject as RenderView).child = child as RenderBox;
  }
  moveRenderObjectChild(): void {}
  removeRenderObjectChild(): void {
    (this.renderObject as RenderView).child = null;
  }
}

/**
 * 框架总绑定(对标 Flutter WidgetsFlutterBinding):连接帧回调与视图事件、调度器、手势、渲染管线、构建、焦点、服务。
 * 每帧 drawFrame:build → layout → compositingBits → paint → 合成 Scene → 各视图 render → 卸载 inactive Element。
 *
 * 视图: element 树只有一棵, 渲染树每个视图一棵。runApp 把根 view 渲染进隐式视图 0(宿主启动时创建的窗口);
 * runView 不绑定任何视图, 窗口内容全部由 RootView 渲染(含视图 0 的窗口 Widget.fromId(0))。所有窗口都可以
 * 被关闭——隐式视图关闭后根 view 照常构建(其他窗口挂在它下面), 只是不再渲染到任何地方。
 * 窗口的外观与行为(标题、光标、关闭…)是 Widget 对象的方法与事件(宿主的 momo/window 通道), 不经引擎。
 */
export class ViewsBinding {
  static #instance: ViewsBinding | null = null;

  /** 单例(首次调用时初始化)。 */
  static ensureInitialized(): ViewsBinding {
    if (ViewsBinding.#instance === null) ViewsBinding.#instance = new ViewsBinding();
    return ViewsBinding.#instance;
  }
  static get instance(): ViewsBinding {
    return ViewsBinding.ensureInitialized();
  }

  readonly scheduler = SchedulerBinding.instance;
  readonly gestures = GestureBinding.instance;
  readonly pipelineOwner: PipelineOwner;
  readonly buildOwner: BuildOwner;
  readonly renderView: RenderView;
  readonly mouseTracker: MouseTracker;
  readonly focusManager = FocusManager.instance;
  #root: RootElement | null = null;
  #viewOverride: ViewConfiguration | null = null;
  readonly #sceneBuilder = new SceneBuilder();
  readonly #observers: ViewsBindingObserver[] = [];
  #framesRendered = 0;
  #inDrawFrame = false;
  readonly #viewHosts = new Map<number, ViewRenderHost>();
  readonly #viewsListeners = new Set<() => void>();
  /** 根 view 渲染进隐式视图(runApp);runView 时为 false。 */
  #implicit = true;
  /** 已安装隐式视图事件处理器的视图对象。 */
  #implicitView: Viewport | null = null;

  private constructor() {
    const scheduler = this.scheduler;
    this.pipelineOwner = new PipelineOwner(() => scheduler.ensureVisualUpdate());
    this.buildOwner = new BuildOwner(() => scheduler.ensureVisualUpdate());
    this.renderView = new RenderView(this.#createViewConfiguration());
    this.pipelineOwner.rootNode = this.renderView;
    this.renderView.prepareInitialFrame();
    scheduler.addPersistentFrameCallback(() => this.drawFrame());

    const gestures = this.gestures;
    gestures.hitTestInView = (result, position, viewId) => this.#renderViewOf(viewId)?.hitTest(result, position);
    this.mouseTracker = new MouseTracker((p, viewId) => gestures.hitTest(p, viewId),
                                         (cursor, viewId) => Widget.fromId(viewId)?.setCursor(cursor));
    platformDispatcher.onViewportsChanged = () => {
      // 视图移除发生在帧之后, 之后不一定还有帧: 在这里就丢掉已关闭窗口的状态, 不等下一帧。
      this.#pruneClosedViews();
      for (const l of [...this.#viewsListeners]) {
        try {
          l();
        } catch (e) {
          reportError(e, "in onViewportsChanged");
        }
      }
    };
    gestures.mouseTracker = this.mouseTracker;
    gestures.pointerCaptureSink = (viewId, sequenceId) => Widget.fromId(viewId)?.capturePointer(sequenceId);
    this.#bindImplicitView();
    this.focusManager.registerGlobalHandlers();
    app.on("state-changed", this.#onAppState);
    TextInput.ensureInitialized();
  }

  // 应用被隐藏(或正在退出)时不出帧;窗口的最小化/隐藏由引擎按视图处理。
  #onAppState = (state: AppState): void => {
    this.scheduler.framesEnabled = state !== "suspended" && state !== "terminating";
  };

  /**
   * 根 view 是否渲染进隐式视图。runApp 为 true;runView 为 false——此时视图 0 像其他窗口一样由
   * RootView({ widget: Widget.fromId(0) }) 渲染。
   */
  useImplicitView(on: boolean): void {
    if (on === this.#implicit) return;
    this.#implicit = on;
    this.#bindImplicitView();
    this.handleMetricsChanged();
  }

  /** 隐式视图(视图 0)的事件进入根渲染树(runApp)。视图 0 不存在或 runView 时不安装。 */
  #bindImplicitView(): void {
    const view = this.#implicit ? viewports.get(kImplicitViewId) ?? null : null;
    if (view === this.#implicitView) return;
    this.#unbindImplicitView();
    if (view === null) return;
    this.#implicitView = view;
    const gestures = this.gestures;
    view.onMetricsChanged = () => this.handleMetricsChanged();
    view.onPointerPacket = (data: Float64Array, count: number) => {
      try {
        gestures.handlePointerPacket(data, count, this.renderView.configuration.devicePixelRatio, view.id);
      } catch (e) {
        reportError(e, "while handling a pointer packet");
      }
    };
    view.onKeyData = (data: KeyData) => {
      try {
        return HardwareKeyboard.instance.handleKeyData(data);
      } catch (e) {
        reportError(e, "while handling key data");
        return false;
      }
    };
    view.onFocusChanged = (focused: boolean) => {
      if (!focused) HardwareKeyboard.instance.clearState();
      for (const l of [...this.#implicitFocusListeners]) l(focused);
    };
  }

  readonly #implicitFocusListeners = new Set<(focused: boolean) => void>();
  /** @internal 隐式视图的系统焦点变化(AppRoot 据此恢复焦点作用域)。 */
  addImplicitFocusListener(listener: (focused: boolean) => void): void {
    this.#implicitFocusListeners.add(listener);
  }
  removeImplicitFocusListener(listener: (focused: boolean) => void): void {
    this.#implicitFocusListeners.delete(listener);
  }

  #unbindImplicitView(): void {
    const view = this.#implicitView;
    if (view === null) return;
    this.#implicitView = null;
    for (const k of ["onMetricsChanged", "onPointerPacket", "onKeyData", "onFocusChanged"] as const) {
      (view as unknown as Record<string, unknown>)[k] = null;
    }
  }

  /** 视图 viewId 的渲染树根(隐式视图为根渲染树;已关闭或未注册时为 null)。 */
  #renderViewOf(viewId: number): RenderView | null {
    if (viewId === kImplicitViewId && this.#implicit) return viewports.has(viewId) ? this.renderView : null;
    return this.#viewHosts.get(viewId)?.renderView ?? null;
  }

  #createViewConfiguration(): ViewConfiguration {
    if (this.#viewOverride !== null) return this.#viewOverride;
    const view = viewports.get(kImplicitViewId);
    if (view === undefined) return { size: Size.zero, devicePixelRatio: 1 };
    const dpr = view.devicePixelRatio || 1;
    const s = view.physicalSize;
    return { size: new Size(s.width / dpr, s.height / dpr), devicePixelRatio: dpr };
  }

  /** 根 view 是否渲染进隐式视图(runApp 为 true, runView 为 false)。 */
  get rendersImplicitView(): boolean {
    return this.#implicit;
  }

  /** 当前视图配置(逻辑尺寸 + dpr)。 */
  get viewConfiguration(): ViewConfiguration {
    return this.renderView.configuration;
  }

  /** 已渲染帧数。 */
  get framesRendered(): number {
    return this.#framesRendered;
  }

  /** 根 Element(调试/测试)。 */
  get rootElement(): Element | null {
    return this.#root;
  }

  addObserver(o: ViewsBindingObserver): void {
    this.#observers.push(o);
  }
  removeObserver(o: ViewsBindingObserver): void {
    const i = this.#observers.indexOf(o);
    if (i >= 0) this.#observers.splice(i, 1);
  }

  // ---- 多视图 ----

  /** 注册一个非主窗口(RootView 挂载时)。之后每帧与主窗口一起 layout/paint/render。 */
  registerView(host: ViewRenderHost): void {
    this.#viewHosts.set(host.viewport.id, host);
    this.scheduler.ensureVisualUpdate();
  }
  unregisterView(host: ViewRenderHost): void {
    if (this.#viewHosts.get(host.viewport.id) === host) this.#viewHosts.delete(host.viewport.id);
  }
  /** 已注册的非主窗口视图(调试/测试)。 */
  get viewHosts(): ReadonlyMap<number, ViewRenderHost> {
    return this.#viewHosts;
  }
  // 隐式视图关闭后不再引用它(之后的事件不会再来)。
  #pruneClosedViews(): void {
    if (this.#implicitView !== null && viewports.get(this.#implicitView.id) !== this.#implicitView) {
      this.#implicitView = null;
    }
  }
  /** 视图集合变化(createView 完成、窗口被关闭)的监听。 */
  addViewsListener(listener: () => void): void {
    this.#viewsListeners.add(listener);
  }
  removeViewsListener(listener: () => void): void {
    this.#viewsListeners.delete(listener);
  }

  /** 窗口尺寸/缩放变化。 */
  handleMetricsChanged(): void {
    this.renderView.configuration = this.#createViewConfiguration();
    for (const o of this.#observers.slice()) o.didChangeMetrics?.();
    this.scheduler.scheduleFrame();
  }

  /** 测试用:覆盖视图逻辑尺寸与 dpr(null 恢复为窗口值)。 */
  debugOverrideView(size: Size | null, devicePixelRatio = 1): void {
    this.#viewOverride = size === null ? null : { size, devicePixelRatio };
    this.handleMetricsChanged();
  }

  /** 挂载(或替换)根 view。 */
  attachRootView(view: View | null): void {
    const rootView = new BindingRootView(view, this.renderView);
    if (this.#root === null) {
      const root = rootView.createElement() as RootElement;
      root.owner = this.buildOwner;
      this.#root = root;
      this.buildOwner.buildScope(root, () => root.mount(null, null));
    } else {
      const root = this.#root;
      this.buildOwner.buildScope(root, () => root.update(rootView));
    }
    this.scheduler.ensureVisualUpdate();
  }

  /** 每帧的构建-布局-绘制-合成。 */
  drawFrame(): void {
    if (this.#inDrawFrame) return;
    this.#inDrawFrame = true;
    try {
      if (this.#root !== null) this.buildOwner.buildScope(this.#root);
      const hadLayout = this.pipelineOwner._nodesNeedingLayout.length > 0;
      this.pipelineOwner.flushLayout();
      this.pipelineOwner.flushCompositingBits();
      this.pipelineOwner.flushPaint();
      const scene = this.renderView.compositeFrame(this.#sceneBuilder);
      const implicitView = this.#implicit ? viewports.get(kImplicitViewId) : undefined;
      if (implicitView?.visible !== false) implicitView?.render(scene);  // 隐式视图已关闭(或 runView): 不提交
      // 其他窗口: 共用同一次 build, 各自 layout/paint/合成并提交到自己的视图。不可见的窗口(隐藏、最小化)跳过:
      // 引擎不为它出帧, 重新可见时引擎补一帧。
      for (const host of [...this.#viewHosts.values()]) {
        const viewport = host.viewport;
        if (!viewports.has(viewport.id) || viewport.visible === false) continue;  // 已关闭(RootView 稍后卸载)或不可见
        const owner = host.pipelineOwner;
        owner.flushLayout();
        owner.flushCompositingBits();
        owner.flushPaint();
        viewport.render(host.renderView.compositeFrame(this.#sceneBuilder));
      }
      this.#framesRendered++;
      this.buildOwner.finalizeTree();
      this.#pruneClosedViews();
      if (hadLayout && this.mouseTracker.mouseIsConnected) {
        this.scheduler.addPostFrameCallback(() => this.mouseTracker.updateAllDevices());
      }
    } finally {
      this.#inDrawFrame = false;
    }
  }

  /** 请求首帧。 */
  scheduleWarmUpFrame(): void {
    this.scheduler.scheduleFrame();
  }

  /** 测试用:卸载整棵树并清空手势/焦点/调度状态。 */
  debugResetForTest(): void {
    this.attachRootView(null);
    this.drawFrame();
    this.#implicit = true;
    this.#unbindImplicitView();
    Widget.debugReset();
    app.debugReset();
    app.on("state-changed", this.#onAppState);
    this.#implicitFocusListeners.clear();
    this.#bindImplicitView();
    this.#viewHosts.clear();
    this.#viewsListeners.clear();
    this.gestures.debugReset();
    this.mouseTracker.reset();
    this.focusManager.debugReset();
    this.scheduler.debugResetForTest();
    HardwareKeyboard.instance.clearState();
    TextInput.debugReset();
  }
}

/** 应用根:MediaQuery + 默认快捷键/行为 + 根焦点作用域 + Overlay。 */
class AppRoot extends StatefulView {
  constructor(readonly app: View, key?: Key | null) {
    super(key);
  }
  createState(): State {
    return new AppRootState();
  }
}

class AppRootState extends State<AppRoot> implements ViewsBindingObserver {
  #entry: OverlayEntry | null = null;
  #focused = true;
  /**
   * 隐式视图的窗口: 挂载时取一次并固定(窗口之后被关闭也不换), 子树结构因此不随窗口开关而变化——
   * 否则挂在根下的其他窗口会被重建。
   */
  #widget: Widget | null = Widget.fromId(kImplicitViewId);
  readonly #onControlsChanged = (): void => {
    if (this.mounted) this.setState();
  };
  readonly #scope = new FocusScopeNode({ debugLabel: "App Scope" });
  readonly #shortcuts = defaultShortcuts();
  readonly #actions = defaultActions();

  override initState(): void {
    ViewsBinding.instance.addObserver(this);
    ViewsBinding.instance.addImplicitFocusListener(this.#onFocusChanged);
    this.#widget?.on("controls-changed", this.#onControlsChanged);
    this.#entry = new OverlayEntry({ builder: () => this.view.app, opaque: true });
  }
  override didUpdateView(): void {
    this.#entry?.markNeedsBuild();
  }
  override dispose(): void {
    ViewsBinding.instance.removeObserver(this);
    ViewsBinding.instance.removeImplicitFocusListener(this.#onFocusChanged);
    this.#widget?.off("controls-changed", this.#onControlsChanged);
    this.#scope.dispose();
  }
  didChangeMetrics(): void {
    this.setState();
  }
  readonly #onFocusChanged = (focused: boolean): void => {
    // 多窗口: 主窗口重新获得焦点时, 把 momo 焦点从其他窗口拉回(恢复本窗口上次的焦点)。
    if (focused && !this.#scope.hasFocus) this.#scope.requestFocus();
    if (this.mounted) this.setState(() => (this.#focused = focused));
  };
  build(_context: BuildContext): View {
    const cfg = ViewsBinding.instance.viewConfiguration;
    const widget = this.#widget;
    const controls = widget?.controls ?? null;
    const content = new MediaQuery({
      data: new MediaQueryData({
        size: cfg.size, devicePixelRatio: cfg.devicePixelRatio, windowFocused: this.#focused,
        windowControls: controls !== null ? Rect.fromLTWH(controls.x, controls.y, controls.width, controls.height) : null,
      }),
      child: new Shortcuts({
        debugLabel: "Default Shortcuts",
        shortcuts: this.#shortcuts,
        child: new Actions({
          actions: this.#actions,
          child: new FocusScope({
            node: this.#scope,
            autofocus: true,
            child: new Overlay({ initialEntries: [this.#entry!] }),
          }),
        }),
      }),
    });
    return widget !== null ? new WidgetScope({ widget, child: content }) : content;
  }
}

/**
 * 启动应用: 以 app 为根构建 view 树, 渲染进隐式视图(宿主启动时创建的窗口), 并请求首帧。
 * 其他窗口用 Widget.create 打开, 内容用 ViewAnchor + RootView 挂在树里。
 */
export function runApp(app: View): ViewsBinding {
  const binding = ViewsBinding.ensureInitialized();
  binding.useImplicitView(true);
  binding.attachRootView(new AppRoot(app));
  binding.scheduleWarmUpFrame();
  return binding;
}

/**
 * 启动应用(多窗口, 所有窗口对等): 根 view 不绑定任何视图, 每个窗口的内容由 RootView 渲染——启动时宿主创建的
 * 窗口是 Widget.fromId(0)(同 Flutter 的 runWidget + View)。任何窗口都可以关闭;最后一个窗口关闭后应用照常运行
 * (app 'window-all-closed'), 是否退出由应用决定。根 view 本身不应产生渲染对象(通常是
 * ViewAnchor({ views: [...] }), 可包在 MaterialApp 等只提供主题的 view 里)。
 */
export function runView(view: View): ViewsBinding {
  const binding = ViewsBinding.ensureInitialized();
  binding.useImplicitView(false);
  binding.attachRootView(view);
  binding.scheduleWarmUpFrame();
  return binding;
}
