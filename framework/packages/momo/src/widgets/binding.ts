import { SceneBuilder, views, window, type KeyData, type View as UiView } from "momo:ui";
import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { GestureBinding } from "../gestures/binding";
import { Rect, Size } from "../painting/geometry";
import { MouseTracker } from "../rendering/mouse_tracker";
import { PipelineOwner } from "../rendering/object";
import { RenderView, type ViewConfiguration } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import { AppLifecycle, type AppLifecycleState } from "../services/lifecycle";
import { TextInput } from "../services/text_input";
import { WindowService } from "../services/window";
import { Actions, Shortcuts, defaultActions, defaultShortcuts } from "./actions";
import { FocusManager, FocusScopeNode } from "./focus_manager";
import { FocusScope } from "./focus_scope";
import {
  BuildOwner, Element, RenderObjectElement, RenderObjectWidget, State, StatefulWidget, type BuildContext, type Widget,
} from "./framework";
import { MediaQuery, MediaQueryData } from "./media_query";
import { Overlay, OverlayEntry } from "./overlay";
import { ViewScope } from "./view_scope";
import type { RenderObject } from "../rendering/object";
import type { RenderBox } from "../rendering/box";

/** 应用级事件观察者。didChangeMetrics / didChangeWindowFocus / didRequestClose 针对隐式视图(runApp 渲染的窗口)。 */
export interface WidgetsBindingObserver {
  didChangeMetrics?(): void;
  didChangeAppLifecycleState?(state: AppLifecycleState): void;
  didChangeWindowFocus?(focused: boolean): void;
  /** 用户点了隐式视图窗口的关闭按钮: 返回 false 阻止关闭。 */
  didRequestClose?(): boolean | void;
  /** 用户/系统请求退出应用(Cmd+Q 等): 返回 false 取消退出。 */
  didRequestAppExit?(): boolean | void;
}

/** 隐式视图 id(宿主启动时创建的窗口)。 */
export const kImplicitViewId = 0;

/** 一个视图在渲染侧的状态(由 View widget 注册;隐式视图由 runApp 的根渲染, 不在这里)。 */
export interface ViewRenderHost {
  readonly view: UiView;
  readonly renderView: RenderView;
  readonly pipelineOwner: PipelineOwner;
}

/** 根 widget:把子树挂到 RenderView。 */
class RootWidget extends RenderObjectWidget {
  constructor(readonly child: Widget | null, readonly view: RenderView) {
    super();
  }
  createElement(): Element {
    return new RootElement(this);
  }
  createRenderObject(): RenderObject {
    return this.view;
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
    this.#child = this.updateChild(this.#child, (this.widget as RootWidget).child, null);
  }
  override update(newWidget: Widget): void {
    super.update(newWidget);
    this.#child = this.updateChild(this.#child, (this.widget as RootWidget).child, null);
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
 * 视图: element 树只有一棵, 渲染树每个视图一棵。runApp 把根 widget 渲染进隐式视图 0(宿主启动时创建的窗口);
 * runWidget 不绑定任何视图, 窗口全部由 Window / View 声明(含经 Window({ view }) 接管的视图 0)。所有视图都可以
 * 被关闭——隐式视图关闭后根 widget 照常构建(其他窗口挂在它下面), 只是不再渲染到任何地方。
 * 窗口的外观与行为(标题、光标、关闭询问…)经 WindowService(宿主的 momo/window 通道), 不经引擎。
 */
export class WidgetsBinding {
  static #instance: WidgetsBinding | null = null;

  /** 单例(首次调用时初始化)。 */
  static ensureInitialized(): WidgetsBinding {
    if (WidgetsBinding.#instance === null) WidgetsBinding.#instance = new WidgetsBinding();
    return WidgetsBinding.#instance;
  }
  static get instance(): WidgetsBinding {
    return WidgetsBinding.ensureInitialized();
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
  readonly #observers: WidgetsBindingObserver[] = [];
  #framesRendered = 0;
  #inDrawFrame = false;
  readonly #viewHosts = new Map<number, ViewRenderHost>();
  readonly #viewsListeners = new Set<() => void>();
  /** 根 widget 渲染进隐式视图(runApp);runWidget 时为 false。 */
  #implicit = true;
  /** 已安装隐式视图事件处理器的视图对象。 */
  #implicitView: UiView | null = null;

  private constructor() {
    const scheduler = this.scheduler;
    this.pipelineOwner = new PipelineOwner(() => scheduler.ensureVisualUpdate());
    this.buildOwner = new BuildOwner(() => scheduler.ensureVisualUpdate());
    this.renderView = new RenderView(this.#createViewConfiguration());
    this.pipelineOwner.rootNode = this.renderView;
    this.renderView.prepareInitialFrame();
    scheduler.addPersistentFrameCallback(() => this.drawFrame());

    const gestures = this.gestures;
    const windows = WindowService.instance;
    windows.ensureInitialized();
    gestures.hitTestInView = (result, position, viewId) => this.#renderViewOf(viewId)?.hitTest(result, position);
    this.mouseTracker = new MouseTracker((p, viewId) => gestures.hitTest(p, viewId),
                                         (cursor, viewId) => windows.setCursor(viewId, cursor));
    window.onViewsChanged = () => {
      // 视图移除发生在帧之后, 之后不一定还有帧: 在这里就丢掉已关闭窗口的状态, 不等下一帧。
      this.#pruneClosedViews();
      for (const l of [...this.#viewsListeners]) {
        try {
          l();
        } catch (e) {
          reportError(e, "in onViewsChanged");
        }
      }
    };
    gestures.mouseTracker = this.mouseTracker;
    gestures.pointerCaptureSink = (viewId, sequenceId) => windows.capturePointer(viewId, sequenceId);
    this.#installAppExitHandler();
    this.#bindImplicitView();
    this.focusManager.registerGlobalHandlers();
    AppLifecycle.instance.ensureInitialized();
    AppLifecycle.instance.addListener(this.#onLifecycle);
    TextInput.ensureInitialized();
  }

  #onLifecycle = (state: AppLifecycleState): void => {
    this.scheduler.framesEnabled = state === "resumed";
    for (const o of this.#observers.slice()) o.didChangeAppLifecycleState?.(state);
  };

  #installAppExitHandler(): void {
    WindowService.instance.setAppExitHandler(() => {
      for (const o of this.#observers.slice()) {
        try {
          if (o.didRequestAppExit?.() === false) return false;
        } catch (e) {
          reportError(e, "in didRequestAppExit");
        }
      }
      return true;
    });
  }

  /**
   * 根 widget 是否渲染进隐式视图。runApp 为 true;runWidget 为 false——此时视图 0 像其他窗口一样由
   * Window({ view }) / View 接管。
   */
  useImplicitView(on: boolean): void {
    if (on === this.#implicit) return;
    this.#implicit = on;
    this.#bindImplicitView();
    this.handleMetricsChanged();
  }

  /** 隐式视图(视图 0)的事件进入根渲染树(runApp)。视图 0 不存在或 runWidget 时不安装。 */
  #bindImplicitView(): void {
    const view = this.#implicit ? views.get(kImplicitViewId) ?? null : null;
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
      for (const o of this.#observers.slice()) o.didChangeWindowFocus?.(focused);
    };
    WindowService.instance.setCloseRequestHandler(view.id, () => {
      for (const o of this.#observers.slice()) {
        try {
          if (o.didRequestClose?.() === false) return false;
        } catch (e) {
          reportError(e, "in didRequestClose");
        }
      }
      return true;
    });
  }

  #unbindImplicitView(): void {
    const view = this.#implicitView;
    if (view === null) return;
    this.#implicitView = null;
    for (const k of ["onMetricsChanged", "onPointerPacket", "onKeyData", "onFocusChanged"] as const) {
      (view as unknown as Record<string, unknown>)[k] = null;
    }
    WindowService.instance.setCloseRequestHandler(view.id, null);
  }

  /** 视图 viewId 的渲染树根(隐式视图为根渲染树;已关闭或未注册时为 null)。 */
  #renderViewOf(viewId: number): RenderView | null {
    if (viewId === kImplicitViewId && this.#implicit) return views.has(viewId) ? this.renderView : null;
    return this.#viewHosts.get(viewId)?.renderView ?? null;
  }

  #createViewConfiguration(): ViewConfiguration {
    if (this.#viewOverride !== null) return this.#viewOverride;
    const view = views.get(kImplicitViewId);
    if (view === undefined) return { size: Size.zero, devicePixelRatio: 1 };
    const dpr = view.devicePixelRatio || 1;
    const s = view.physicalSize;
    return { size: new Size(s.width / dpr, s.height / dpr), devicePixelRatio: dpr };
  }

  /** 根 widget 是否渲染进隐式视图(runApp 为 true, runWidget 为 false)。 */
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

  addObserver(o: WidgetsBindingObserver): void {
    this.#observers.push(o);
  }
  removeObserver(o: WidgetsBindingObserver): void {
    const i = this.#observers.indexOf(o);
    if (i >= 0) this.#observers.splice(i, 1);
  }

  // ---- 多视图 ----

  /** 注册一个非主窗口视图(View widget 挂载时)。之后每帧与主窗口一起 layout/paint/render。 */
  registerView(host: ViewRenderHost): void {
    this.#viewHosts.set(host.view.id, host);
    this.scheduler.ensureVisualUpdate();
  }
  unregisterView(host: ViewRenderHost): void {
    if (this.#viewHosts.get(host.view.id) === host) this.#viewHosts.delete(host.view.id);
  }
  /** 已注册的非主窗口视图(调试/测试)。 */
  get viewHosts(): ReadonlyMap<number, ViewRenderHost> {
    return this.#viewHosts;
  }
  // 已关闭的视图: 丢掉窗口服务里的状态与处理器;隐式视图关闭后不再引用它(之后的事件不会再来)。
  #pruneClosedViews(): void {
    if (this.#implicitView !== null && views.get(this.#implicitView.id) !== this.#implicitView) {
      WindowService.instance.forget(this.#implicitView.id);
      this.#implicitView = null;
    }
    for (const id of [...this.#knownViews]) {
      if (!views.has(id)) {
        this.#knownViews.delete(id);
        WindowService.instance.forget(id);
      }
    }
    for (const id of views.keys()) this.#knownViews.add(id);
  }
  readonly #knownViews = new Set<number>(views.keys());
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

  /** 挂载(或替换)根 widget。 */
  attachRootWidget(widget: Widget | null): void {
    const rootWidget = new RootWidget(widget, this.renderView);
    if (this.#root === null) {
      const root = rootWidget.createElement() as RootElement;
      root.owner = this.buildOwner;
      this.#root = root;
      this.buildOwner.buildScope(root, () => root.mount(null, null));
    } else {
      const root = this.#root;
      this.buildOwner.buildScope(root, () => root.update(rootWidget));
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
      const implicitView = this.#implicit ? views.get(kImplicitViewId) : undefined;
      implicitView?.render(scene);  // 隐式视图已关闭(或 runWidget): 根渲染树不提交到任何视图
      // 其他窗口: 共用同一次 build, 各自 layout/paint/合成并提交到自己的视图。
      for (const host of [...this.#viewHosts.values()]) {
        if (!views.has(host.view.id)) continue;  // 宿主已关闭(View widget 稍后卸载)
        const owner = host.pipelineOwner;
        owner.flushLayout();
        owner.flushCompositingBits();
        owner.flushPaint();
        host.view.render(host.renderView.compositeFrame(this.#sceneBuilder));
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
    this.attachRootWidget(null);
    this.drawFrame();
    this.#implicit = true;
    this.#unbindImplicitView();
    WindowService.instance.debugReset();
    this.#installAppExitHandler();
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
class AppRoot extends StatefulWidget {
  constructor(readonly app: Widget, key?: Key | null) {
    super(key);
  }
  createState(): State {
    return new AppRootState();
  }
}

class AppRootState extends State<AppRoot> implements WidgetsBindingObserver {
  #entry: OverlayEntry | null = null;
  #focused = true;
  /**
   * 隐式视图对象: 挂载时取一次并固定(视图之后被关闭也不换), 子树结构因此不随视图开关而变化——
   * 否则挂在根下的其他窗口会被重建。
   */
  #view: UiView | null = views.get(kImplicitViewId) ?? null;
  readonly #onWindowChange = (view: number): void => {
    if (view === kImplicitViewId && this.mounted) this.setState();
  };
  readonly #scope = new FocusScopeNode({ debugLabel: "App Scope" });
  readonly #shortcuts = defaultShortcuts();
  readonly #actions = defaultActions();

  override initState(): void {
    WidgetsBinding.instance.addObserver(this);
    WindowService.instance.addListener(this.#onWindowChange);
    this.#entry = new OverlayEntry({ builder: () => this.widget.app, opaque: true });
  }
  override didUpdateWidget(): void {
    this.#entry?.markNeedsBuild();
  }
  override dispose(): void {
    WidgetsBinding.instance.removeObserver(this);
    WindowService.instance.removeListener(this.#onWindowChange);
    this.#scope.dispose();
  }
  didChangeMetrics(): void {
    this.setState();
  }
  didChangeWindowFocus(focused: boolean): void {
    // 多窗口: 主窗口重新获得焦点时, 把 momo 焦点从其他窗口拉回(恢复本窗口上次的焦点)。
    if (focused && !this.#scope.hasFocus) this.#scope.requestFocus();
    this.setState(() => (this.#focused = focused));
  }
  build(_context: BuildContext): Widget {
    const cfg = WidgetsBinding.instance.viewConfiguration;
    const view = this.#view;
    const controls = view !== null ? WindowService.instance.controlsOf(view.id) : null;
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
    return view !== null ? new ViewScope({ view, child: content }) : content;
  }
}

/**
 * 启动应用: 以 app 为根构建 widget 树, 渲染进隐式视图(宿主启动时创建的窗口), 并请求首帧。
 * 其他窗口用 ViewAnchor + Window 声明。
 */
export function runApp(app: Widget): WidgetsBinding {
  const binding = WidgetsBinding.ensureInitialized();
  binding.useImplicitView(true);
  binding.attachRootWidget(new AppRoot(app));
  binding.scheduleWarmUpFrame();
  return binding;
}

/**
 * 启动应用(多窗口, 所有窗口对等): 根 widget 不绑定任何视图, 窗口全部由 Window / View 声明——启动时宿主创建的
 * 窗口用 Window({ view: views.get(0) }) 接管(同 Flutter 的 runWidget + View)。任何窗口都可以关闭;最后一个
 * 窗口关闭后宿主退出应用。根 widget 本身不应产生渲染对象(通常是 ViewAnchor({ views: [...] }), 可包在
 * MaterialApp 等只提供主题的 widget 里)。
 */
export function runWidget(widget: Widget): WidgetsBinding {
  const binding = WidgetsBinding.ensureInitialized();
  binding.useImplicitView(false);
  binding.attachRootWidget(widget);
  binding.scheduleWarmUpFrame();
  return binding;
}
