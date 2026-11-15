import { SceneBuilder, viewports, platformDispatcher, type Viewport } from "momo:ui";
import { reportError } from "../foundation/assert";
import { GestureBinding } from "../gestures/binding";
import { Size } from "../painting/geometry";
import { ContainerLayer, TextureLayer, type Layer } from "../rendering/layer";
import { MouseTracker } from "../rendering/mouse_tracker";
import type { PipelineOwner } from "../rendering/object";
import type { RenderView, ViewConfiguration } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import { app, type AppState } from "../services/app";
import { TextInput } from "../services/text_input";
import { Widget } from "../services/widget";
import { FocusManager } from "./focus_manager";
import { BuildOwner, Element, View } from "./framework";

/** 启动视图 id(宿主启动时创建的窗口, runApp 渲染到这里)。 */
export const kImplicitViewId = 0;

/** 一个窗口在渲染侧的状态(由 RootView 注册): 每个窗口一棵渲染树。 */
export interface ViewRenderHost {
  readonly viewport: Viewport;
  readonly renderView: RenderView;
  readonly pipelineOwner: PipelineOwner;
}

/** element 树的根: 自己没有渲染对象(渲染树的根是各窗口 RootView 的 RenderView)。 */
class RootHolder extends View {
  constructor(readonly child: View | null) {
    super();
  }
  createElement(): Element {
    return new RootElement(this);
  }
}

class RootElement extends Element {
  #child: Element | null = null;
  override visitChildren(visitor: (child: Element) => void): void {
    if (this.#child !== null) visitor(this.#child);
  }
  override forgetChild(child: Element): void {
    if (child === this.#child) this.#child = null;
  }
  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.#rebuildChild();
  }
  override update(newView: View): void {
    super.update(newView);
    this.#rebuildChild();
  }
  protected performRebuild(): void {
    this.dirty = false;
    this.#rebuildChild();
  }
  #rebuildChild(): void {
    this.#child = this.updateChild(this.#child, (this.view as RootHolder).child, null);
  }
}

function hasTextureLayer(layer: Layer): boolean {
  if (layer instanceof TextureLayer) return true;
  return layer instanceof ContainerLayer && layer.children.some(hasTextureLayer);
}

/**
 * 框架总绑定(对标 Flutter WidgetsFlutterBinding):连接帧回调与视图事件、调度器、手势、渲染管线、构建、焦点、服务。
 * 每帧 drawFrame:build → 各窗口 layout → compositingBits → paint → 合成 Scene → render → 卸载 inactive Element。
 *
 * 视图: element 树只有一棵, 渲染树每个窗口一棵。绑定本身不渲染任何窗口: 窗口内容全部由 RootView 渲染并在这里
 * 登记(registerView);runApp 就是在启动窗口(视图 0)上的 RootView。所有窗口都可以被关闭——窗口关闭后它的
 * RootView 照常构建(其他窗口可能挂在它下面), 只是不再布局、绘制与提交。
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
  readonly buildOwner: BuildOwner;
  readonly mouseTracker: MouseTracker;
  readonly focusManager = FocusManager.instance;
  #root: RootElement | null = null;
  #rootView: View | null = null;
  #viewOverride: ViewConfiguration | null = null;
  readonly #sceneBuilder = new SceneBuilder();
  #framesRendered = 0;
  #inDrawFrame = false;
  readonly #viewHosts = new Map<number, ViewRenderHost>();
  /** 即使没有脏节点, 下一帧也要提交画面的窗口(首帧、尺寸变化、重新可见)。 */
  readonly #needsCommit = new Set<ViewRenderHost>();
  /** 画面里有外部纹理的窗口: 纹理内容的更新不经过渲染树, 每帧都提交。 */
  readonly #liveContent = new Set<ViewRenderHost>();
  readonly #viewsListeners = new Set<() => void>();

  private constructor() {
    const scheduler = this.scheduler;
    this.buildOwner = new BuildOwner(() => scheduler.ensureVisualUpdate());
    scheduler.addPersistentFrameCallback(() => this.drawFrame());

    const gestures = this.gestures;
    gestures.hitTestInView = (result, position, viewId) => this.#renderViewOf(viewId)?.hitTest(result, position);
    this.mouseTracker = new MouseTracker((p, viewId) => gestures.hitTest(p, viewId),
                                         (cursor, viewId) => Widget.fromId(viewId)?.setCursor(cursor));
    platformDispatcher.onViewportsChanged = () => {
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
    this.focusManager.registerGlobalHandlers();
    app.on("state-changed", this.#onAppState);
    TextInput.ensureInitialized();
  }

  // 应用被隐藏(或正在退出)时不出帧;窗口的最小化/隐藏由引擎按视图处理。
  #onAppState = (state: AppState): void => {
    this.scheduler.framesEnabled = state !== "suspended" && state !== "terminating";
  };

  /** 视图 viewId 的渲染树根(已关闭或没有 RootView 渲染它时为 null)。 */
  #renderViewOf(viewId: number): RenderView | null {
    const host = this.#viewHosts.get(viewId);
    return host !== undefined && viewports.get(viewId) === host.viewport ? host.renderView : null;
  }

  /** 视图的配置(逻辑尺寸 + dpr);RootView 用它建立与更新窗口的 RenderView。 */
  configurationOf(viewport: Viewport): ViewConfiguration {
    if (viewport.id === kImplicitViewId && this.#viewOverride !== null) return this.#viewOverride;
    const dpr = viewport.devicePixelRatio || 1;
    const s = viewport.physicalSize;
    return { size: new Size(s.width / dpr, s.height / dpr), devicePixelRatio: dpr };
  }

  /** 已渲染帧数。 */
  get framesRendered(): number {
    return this.#framesRendered;
  }

  /** 根 Element(调试/测试)。 */
  get rootElement(): Element | null {
    return this.#root;
  }

  /** 当前挂载的根 view(attachRootView 传入的;未挂载时为 null)。 */
  get rootView(): View | null {
    return this.#rootView;
  }

  // ---- 窗口 ----

  /** 登记一个窗口的渲染树(RootView 挂载时)。之后每帧 layout/paint, 画面有变化时提交到它的视图。 */
  registerView(host: ViewRenderHost): void {
    this.#viewHosts.set(host.viewport.id, host);
    this.#needsCommit.add(host);
    this.scheduler.ensureVisualUpdate();
  }
  unregisterView(host: ViewRenderHost): void {
    if (this.#viewHosts.get(host.viewport.id) === host) this.#viewHosts.delete(host.viewport.id);
    this.#needsCommit.delete(host);
    this.#liveContent.delete(host);
  }
  /** 已登记的窗口(键为视图 id)。 */
  get viewHosts(): ReadonlyMap<number, ViewRenderHost> {
    return this.#viewHosts;
  }
  /**
   * 下一帧无论渲染树是否有变化都提交这个窗口的画面, 并请求一帧。渲染树之外的原因使窗口上的画面过期时调用
   * (尺寸/缩放变化、重新可见)。
   */
  markNeedsCommit(host: ViewRenderHost): void {
    if (this.#viewHosts.get(host.viewport.id) !== host) return;
    this.#needsCommit.add(host);
    this.scheduler.scheduleFrame();
  }
  /** 视图集合变化(createView 完成、窗口被关闭)的监听。 */
  addViewsListener(listener: () => void): void {
    this.#viewsListeners.add(listener);
  }
  removeViewsListener(listener: () => void): void {
    this.#viewsListeners.delete(listener);
  }

  /** 测试用:覆盖启动窗口(视图 0)的逻辑尺寸与 dpr(null 恢复为窗口值)。 */
  debugOverrideView(size: Size | null, devicePixelRatio = 1): void {
    this.#viewOverride = size === null ? null : { size, devicePixelRatio };
    viewports.get(kImplicitViewId)?.onMetricsChanged?.();
  }

  /** 挂载(或替换)根 view。根本身不渲染到任何窗口: 窗口内容由树里的 RootView 渲染。 */
  attachRootView(view: View | null): void {
    this.#rootView = view;
    const holder = new RootHolder(view);
    if (this.#root === null) {
      const root = holder.createElement() as RootElement;
      root.owner = this.buildOwner;
      this.#root = root;
      this.buildOwner.buildScope(root, () => root.mount(null, null));
    } else {
      const root = this.#root;
      this.buildOwner.buildScope(root, () => root.update(holder));
    }
    this.scheduler.ensureVisualUpdate();
  }

  /**
   * 每帧的构建-布局-绘制-合成。build 只有一次;各窗口分别 layout/paint, 画面有变化的窗口合成并提交到自己的视图。
   * 不提交的情况: 窗口已关闭(RootView 稍后卸载)或不可见(隐藏、最小化: 引擎不为它出帧), 这两种连布局也跳过;
   * 以及渲染树没有任何脏节点的窗口——引擎保留着它上一次提交的画面。没有脏节点也照常提交的情况:
   * 首帧、尺寸/缩放变化、重新可见(markNeedsCommit), 画面里有外部纹理, 以及不是框架请求的帧(引擎自己发起:
   * 新视图、重新可见、扩展的帧回调…, 此时引擎期待每个窗口的最新画面)。
   */
  drawFrame(): void {
    if (this.#inDrawFrame) return;
    this.#inDrawFrame = true;
    try {
      if (this.#root !== null) this.buildOwner.buildScope(this.#root);
      const commitAll = !this.scheduler.currentFrameWasRequested;
      let hadLayout = false;
      for (const host of [...this.#viewHosts.values()]) {
        const viewport = host.viewport;
        if (viewports.get(viewport.id) !== viewport) continue;
        if (viewport.visible === false) {
          this.#needsCommit.add(host);  // 重新可见后的第一帧补上这期间的变化
          continue;
        }
        const owner = host.pipelineOwner;
        if (owner._nodesNeedingLayout.length > 0) hadLayout = true;
        const dirty = owner.hasDirtyNodes;
        owner.flushLayout();
        owner.flushCompositingBits();
        owner.flushPaint();
        if (!dirty && !commitAll && !this.#needsCommit.has(host) && !this.#liveContent.has(host)) continue;
        this.#needsCommit.delete(host);
        viewport.render(host.renderView.compositeFrame(this.#sceneBuilder));
        const layer = host.renderView.rootLayer;
        if (layer !== null && hasTextureLayer(layer)) this.#liveContent.add(host);
        else this.#liveContent.delete(host);
      }
      this.#framesRendered++;
      this.buildOwner.finalizeTree();
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
    Widget.debugReset();
    app.debugReset();
    app.on("state-changed", this.#onAppState);
    this.#viewHosts.clear();
    this.#needsCommit.clear();
    this.#liveContent.clear();
    this.#viewsListeners.clear();
    this.gestures.debugReset();
    this.mouseTracker.reset();
    this.focusManager.debugReset();
    this.scheduler.debugResetForTest();
    HardwareKeyboard.instance.clearState();
    TextInput.debugReset();
  }
}
