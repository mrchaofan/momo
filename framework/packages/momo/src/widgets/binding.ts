import { SceneBuilder, views, window, type KeyData, type View as UiView } from "momo:ui";
import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { GestureBinding } from "../gestures/binding";
import { Size } from "../painting/geometry";
import { MouseTracker } from "../rendering/mouse_tracker";
import { PipelineOwner } from "../rendering/object";
import { RenderView, type ViewConfiguration } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import { AppLifecycle, type AppLifecycleState } from "../services/lifecycle";
import { TextInput } from "../services/text_input";
import { Actions, Shortcuts, defaultActions, defaultShortcuts } from "./actions";
import { FocusManager, FocusScopeNode } from "./focus_manager";
import { FocusScope } from "./focus_scope";
import {
  BuildOwner, Element, RenderObjectElement, RenderObjectWidget, State, StatefulWidget, type BuildContext, type Widget,
} from "./framework";
import { MediaQuery, MediaQueryData } from "./media_query";
import { Overlay, OverlayEntry } from "./overlay";
import { ViewScope } from "./view_scope";
import { flushDragRegions } from "./window_chrome";
import type { RenderObject } from "../rendering/object";
import type { RenderBox } from "../rendering/box";

/** 应用级事件观察者。 */
export interface WidgetsBindingObserver {
  didChangeMetrics?(): void;
  didChangeAppLifecycleState?(state: AppLifecycleState): void;
  didChangeWindowFocus?(focused: boolean): void;
  /** 返回 false 阻止关闭窗口。 */
  didRequestClose?(): boolean | void;
}

/** 主窗口之外的一个视图在渲染侧的状态(由 View widget 注册)。 */
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
 * 框架总绑定(对标 Flutter WidgetsFlutterBinding):连接 window 回调、调度器、手势、渲染管线、构建、焦点、服务。
 * 每帧 drawFrame:build → layout → compositingBits → paint → 合成 Scene → window.render → 卸载 inactive Element。
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
  readonly #dragRegionsSent = new Map<UiView, string>();

  private constructor() {
    const scheduler = this.scheduler;
    this.pipelineOwner = new PipelineOwner(() => scheduler.ensureVisualUpdate());
    this.buildOwner = new BuildOwner(() => scheduler.ensureVisualUpdate());
    this.renderView = new RenderView(this.#createViewConfiguration());
    this.pipelineOwner.rootNode = this.renderView;
    this.renderView.prepareInitialFrame();
    scheduler.addPersistentFrameCallback(() => this.drawFrame());

    const gestures = this.gestures;
    gestures.hitTestInView = (result, position, viewId) => {
      const view = viewId === 0 ? this.renderView : this.#viewHosts.get(viewId)?.renderView;
      view?.hitTest(result, position);
    };
    this.mouseTracker = new MouseTracker((p) => gestures.hitTest(p), (cursor) => {
      const target = views.get(gestures.currentViewId) ?? window;
      target.setCursor(cursor);
    });
    window.onViewsChanged = () => {
      // 视图移除发生在帧之后, 之后不一定还有帧: 在这里就丢掉已关闭窗口的记录, 不等下一帧。
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
    gestures.platformViewPointerSink = (viewId, platformViewId, nativeEventId, action) => {
      const target = views.get(viewId) ?? window;
      target.dispatchPlatformViewPointer(platformViewId, nativeEventId, action);
    };

    window.onMetricsChanged = () => this.handleMetricsChanged();
    window.onPointerPacket = (data: Float64Array, count: number) => {
      try {
        gestures.handlePointerPacket(data, count, this.renderView.configuration.devicePixelRatio);
      } catch (e) {
        reportError(e, "while handling a pointer packet");
      }
    };
    window.onKeyData = (data: KeyData) => {
      try {
        return HardwareKeyboard.instance.handleKeyData(data);
      } catch (e) {
        reportError(e, "while handling key data");
        return false;
      }
    };
    window.onFocusChanged = (focused: boolean) => {
      if (!focused) HardwareKeyboard.instance.clearState();
      for (const o of this.#observers.slice()) o.didChangeWindowFocus?.(focused);
    };
    window.onCloseRequested = () => {
      for (const o of this.#observers.slice()) {
        try {
          if (o.didRequestClose?.() === false) return false;
        } catch (e) {
          reportError(e, "in didRequestClose");
        }
      }
      return true;
    };
    this.focusManager.registerGlobalHandlers();
    AppLifecycle.instance.ensureInitialized();
    AppLifecycle.instance.addListener(this.#onLifecycle);
    TextInput.ensureInitialized();
  }

  #onLifecycle = (state: AppLifecycleState): void => {
    this.scheduler.framesEnabled = state === "resumed";
    for (const o of this.#observers.slice()) o.didChangeAppLifecycleState?.(state);
  };

  #createViewConfiguration(): ViewConfiguration {
    if (this.#viewOverride !== null) return this.#viewOverride;
    const dpr = window.devicePixelRatio || 1;
    const s = window.physicalSize;
    return { size: new Size(s.width / dpr, s.height / dpr), devicePixelRatio: dpr };
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
  // 已关闭窗口的拖动区域记录要移除: 否则 Map 一直强引用旧 View(对已移除视图 setDragRegions 是空操作, 不会抛错)。
  #pruneClosedViews(): void {
    for (const v of this.#dragRegionsSent.keys()) if (v !== window && views.get(v.id) !== v) this.#dragRegionsSent.delete(v);
  }
  /** @internal 仍记录着拖动区域的视图数(调试/测试)。 */
  get debugDragRegionViewCount(): number {
    return this.#dragRegionsSent.size;
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
      window.render(scene);
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
      flushDragRegions((root) => {
        if (root === this.renderView) return window;
        for (const host of this.#viewHosts.values()) if (host.renderView === root && views.has(host.view.id)) return host.view;
        return null;
      }, this.#dragRegionsSent);
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
  readonly #scope = new FocusScopeNode({ debugLabel: "App Scope" });
  readonly #shortcuts = defaultShortcuts();
  readonly #actions = defaultActions();

  override initState(): void {
    WidgetsBinding.instance.addObserver(this);
    this.#entry = new OverlayEntry({ builder: () => this.widget.app, opaque: true });
  }
  override didUpdateWidget(): void {
    this.#entry?.markNeedsBuild();
  }
  override dispose(): void {
    WidgetsBinding.instance.removeObserver(this);
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
    return new ViewScope({ view: window, child: new MediaQuery({
      data: new MediaQueryData({ size: cfg.size, devicePixelRatio: cfg.devicePixelRatio, windowFocused: this.#focused }),
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
    }) });
  }
}

/** 启动应用:以 app 为根构建 widget 树并请求首帧。 */
export function runApp(app: Widget): WidgetsBinding {
  const binding = WidgetsBinding.ensureInitialized();
  binding.attachRootWidget(new AppRoot(app));
  binding.scheduleWarmUpFrame();
  return binding;
}
