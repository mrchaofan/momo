// 多窗口(对标 Chromium views::internal::RootView / Flutter 的 View 与 ViewAnchor)。一个 JS 运行时驱动多个原生窗口:
//   - view/element 树只有一棵: 其他窗口的内容作为 ViewAnchor 的"旁支"(views)挂在主树里, 因此共享
//     JS 状态、主题等祖先 InheritedView, 也可以用 GlobalKey 把子树从一个窗口移到另一个窗口;
//   - 渲染树每个窗口一棵: RootView 为窗口建立自己的 RenderView 与 PipelineOwner, 每帧由 ViewsBinding
//     layout/paint, 画面有变化时提交到该窗口的视图(momo:ui Viewport.render);
//   - 每个窗口有自己的 MediaQuery(尺寸/dpr/焦点)、Overlay(对话框只出现在本窗口)与根焦点作用域
//     (挂在焦点树根下, 各窗口并列;窗口获得系统焦点时恢复其上次的焦点)。
// 窗口本身(打开、关闭、标题、状态…)是 Widget 对象(services/widget.ts): RootView 只负责把内容渲染进去。
// 窗口渲染只有这一条实现: runApp(app) 就是启动窗口(视图 0)上的 RootView。
//   const win = await Widget.create({ width: 800, height: 600 });
//   ... ViewAnchor({ child, views: [new RootView({ key, widget: win, child: new Editor() })] })
import type { Viewport } from "momo:ui";
import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { Rect, Size } from "../painting/geometry";
import type { RenderBox } from "../rendering/box";
import { PipelineOwner, type RenderObject } from "../rendering/object";
import { RenderView } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import { Widget, type WidgetBounds } from "../services/widget";
import { Actions, Shortcuts, defaultActions, defaultShortcuts } from "./actions";
import { ViewsBinding, kImplicitViewId, type ViewRenderHost } from "./binding";
import { FocusManager, FocusScopeNode } from "./focus_manager";
import { FocusScope } from "./focus_scope";
import {
  Element, RenderObjectElement, RenderObjectView, State, StatefulView, TickerMode, View, type BuildContext,
} from "./framework";
import { MediaQuery, MediaQueryData } from "./media_query";
import { Overlay, OverlayEntry } from "./overlay";
import { WidgetScope } from "./widget_scope";

// ---------------------------------------------------------------------------
// 渲染侧: 不挂到祖先渲染树的 RenderView 根

class RawView extends RenderObjectView {
  constructor(readonly renderView: RenderView, readonly child: View | null) {
    super();
  }
  createElement(): Element {
    return new RawViewElement(this);
  }
  createRenderObject(): RenderObject {
    return this.renderView;
  }
}

/**
 * RenderView 的 Element: 自己的渲染对象不插入任何祖先(它是另一个窗口渲染树的根), 子节点挂到它下面。
 * slot 相关操作因此都是空操作。
 */
class RawViewElement extends RenderObjectElement {
  #child: Element | null = null;
  override visitChildren(visitor: (child: Element) => void): void {
    if (this.#child !== null) visitor(this.#child);
  }
  override forgetChild(child: Element): void {
    if (child === this.#child) this.#child = null;
  }
  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.#child = this.updateChild(this.#child, (this.view as RawView).child, null);
  }
  override update(newView: View): void {
    super.update(newView);
    this.#child = this.updateChild(this.#child, (this.view as RawView).child, null);
  }
  override attachRenderObject(newSlot: unknown): void {
    this.slot = newSlot;
  }
  override detachRenderObject(): void {
    this.slot = null;
  }
  override updateSlot(newSlot: unknown): void {
    this.slot = newSlot;
  }
  insertRenderObjectChild(child: RenderObject): void {
    (this.renderObject as RenderView).child = child as RenderBox;
  }
  moveRenderObjectChild(): void {}
  removeRenderObjectChild(): void {
    (this.renderObject as RenderView).child = null;
  }
}

// ---------------------------------------------------------------------------
// ViewAnchor: child 正常渲染, views 作为旁支(不参与本处渲染)

/**
 * 在 child 所在位置正常渲染 child;views(各窗口的 RootView)挂在 element 树的这里(继承此处的
 * InheritedView 与状态), 但渲染到各自的窗口。
 */
export class ViewAnchor extends View {
  readonly child: View | null;
  readonly views: readonly View[];
  constructor(o: { key?: Key | null; child?: View | null; views?: readonly View[] }) {
    super(o.key);
    this.child = o.child ?? null;
    this.views = o.views ?? [];
  }
  createElement(): Element {
    return new ViewAnchorElement(this);
  }
}

class ViewAnchorElement extends Element {
  #child: Element | null = null;
  #views: Element[] = [];

  override visitChildren(visitor: (child: Element) => void): void {
    if (this.#child !== null) visitor(this.#child);
    for (const v of this.#views) visitor(v);
  }
  override forgetChild(child: Element): void {
    if (child === this.#child) this.#child = null;
    const i = this.#views.indexOf(child);
    if (i >= 0) this.#views.splice(i, 1);
  }
  override mount(parent: Element | null, newSlot: unknown): void {
    super.mount(parent, newSlot);
    this.#rebuildChildren();
  }
  override update(newView: View): void {
    super.update(newView);
    this.#rebuildChildren();
  }
  protected performRebuild(): void {
    this.dirty = false;
    this.#rebuildChildren();
  }
  #rebuildChildren(): void {
    const w = this.view as ViewAnchor;
    this.#child = this.updateChild(this.#child, w.child, this.slot);
    this.#views = this.updateChildren(this.#views, w.views);
  }
  // 本处的渲染对象只来自 child(views 渲染到别处)。
  override _descendantRenderObjectElement(): RenderObjectElement | null {
    return this.#child?._descendantRenderObjectElement() ?? null;
  }
  override attachRenderObject(newSlot: unknown): void {
    this.#child?.attachRenderObject(newSlot);
    this.slot = newSlot;
  }
  override detachRenderObject(): void {
    this.#child?.detachRenderObject();
    this.slot = null;
  }
}

// ---------------------------------------------------------------------------
// RootView: 把 child 渲染进一个窗口

function toRect(b: WidgetBounds | null): Rect | null {
  return b !== null ? Rect.fromLTWH(b.x, b.y, b.width, b.height) : null;
}

/**
 * 把 child 渲染进窗口 widget。放在 {@link ViewAnchor} 的 views 中, 或直接作为 runView 的根。
 * RootView 不打开也不关闭窗口: 窗口关闭('closed')后应用把它从树里移除;留在树里的 RootView 照常构建
 * (子树的状态保留), 只是不再布局、绘制与提交, 子树里的动画也停下({@link TickerMode} 禁用)。
 */
export class RootView extends StatefulView {
  /** 渲染到的窗口。null: 没有窗口——与窗口已关闭的 RootView 一样只构建(runApp 在启动窗口已关闭时这样挂载)。 */
  readonly widget: Widget | null;
  readonly child: View;
  constructor(o: { key?: Key | null; widget: Widget | null; child: View }) {
    super(o.key);
    this.widget = o.widget;
    this.child = o.child;
  }
  createState(): State {
    return new RootViewState();
  }
}

type ViewportHandlers = Pick<Viewport, "onMetricsChanged" | "onPointerPacket" | "onKeyData" | "onFocusChanged" | "onVisibilityChanged">;
const kViewportHandlers = ["onMetricsChanged", "onPointerPacket", "onKeyData", "onFocusChanged", "onVisibilityChanged"] as const;

class RootViewState extends State<RootView> {
  /** 窗口在 ViewsBinding 里的登记;没有窗口时为 null。 */
  #host: ViewRenderHost | null = null;
  #renderView!: RenderView;
  #pipelineOwner!: PipelineOwner;
  #handlers: ViewportHandlers | null = null;
  #entry: OverlayEntry | null = null;
  #scope = new FocusScopeNode({ debugLabel: "RootView Scope" });
  readonly #shortcuts = defaultShortcuts();
  readonly #actions = defaultActions();
  #focused = false;
  /** 窗口是否还开着(有窗口且未关闭): 否则画面没有人看, 子树里的 Ticker 静音。 */
  #open = false;
  readonly #onWidgetChanged = (): void => {
    if (this.mounted) this.setState();
  };
  readonly #onWidgetClosed = (): void => {
    if (this.mounted) this.setState(() => (this.#open = false));
  };

  override initState(): void {
    const widget = this.view.widget;
    const viewport = widget?.viewport ?? null;
    if (widget !== null && viewport === null) throw new Error(`RootView: window ${widget.id} is already closed`);
    const binding = ViewsBinding.instance;
    const scheduler = SchedulerBinding.instance;
    // 没有窗口: 渲染树永远不会被处理, 也就不为它请求帧。
    this.#pipelineOwner = new PipelineOwner(viewport !== null ? () => scheduler.ensureVisualUpdate() : null);
    this.#renderView = new RenderView(
      viewport !== null ? binding.configurationOf(viewport) : { size: Size.zero, devicePixelRatio: 1 });
    this.#pipelineOwner.rootNode = this.#renderView;
    this.#renderView.prepareInitialFrame();
    this.#entry = new OverlayEntry({ builder: () => this.view.child, opaque: true });
    if (widget === null || viewport === null) return;
    this.#host = { viewport, renderView: this.#renderView, pipelineOwner: this.#pipelineOwner };
    binding.registerView(this.#host);
    this.#focused = viewport.focused;
    this.#open = true;
    widget.on("closed", this.#onWidgetClosed);
    widget.on("controls-changed", this.#onWidgetChanged);
    widget.on("state-changed", this.#onWidgetChanged);
    this.#bind(viewport, this.#host);
  }

  override didUpdateView(old: RootView): void {
    if (old.widget !== this.view.widget) {
      throw new Error("RootView: the widget of a mounted RootView cannot change (use a different key)");
    }
    this.#entry?.markNeedsBuild();
  }

  override dispose(): void {
    const host = this.#host;
    if (host !== null) {
      // 只摘掉自己装的处理器: 同一个窗口换了一个 RootView 渲染时, 新的先挂载、旧的后卸载。
      const viewport = host.viewport;
      for (const k of kViewportHandlers) {
        if (viewport[k] === this.#handlers![k]) viewport[k] = null;
      }
      this.view.widget?.off("closed", this.#onWidgetClosed);
      this.view.widget?.off("controls-changed", this.#onWidgetChanged);
      this.view.widget?.off("state-changed", this.#onWidgetChanged);
      ViewsBinding.instance.unregisterView(host);
    }
    this.#pipelineOwner.rootNode = null;
    this.#scope.dispose();
  }

  #bind(viewport: Viewport, host: ViewRenderHost): void {
    const binding = ViewsBinding.instance;
    const gestures = binding.gestures;
    const handlers: ViewportHandlers = {
      onMetricsChanged: () => {
        host.renderView.configuration = binding.configurationOf(viewport);
        binding.markNeedsCommit(host);
        if (this.mounted) this.setState();
      },
      onPointerPacket: (data: Float64Array, count: number) => {
        try {
          gestures.handlePointerPacket(data, count, host.renderView.configuration.devicePixelRatio, viewport.id);
        } catch (e) {
          reportError(e, "while handling a pointer packet");
        }
      },
      onKeyData: (data) => {
        try {
          return HardwareKeyboard.instance.handleKeyData(data);
        } catch (e) {
          reportError(e, "while handling key data");
          return false;
        }
      },
      onFocusChanged: (focused: boolean) => {
        if (!focused) HardwareKeyboard.instance.clearState();
        // 窗口获得系统焦点: 把 momo 焦点移到本窗口(恢复其上次的焦点)。
        if (focused && !this.#scope.hasFocus) this.#scope.requestFocus();
        if (this.mounted) this.setState(() => (this.#focused = focused));
      },
      // 不可见期间不提交画面: 重新可见时补一帧。
      onVisibilityChanged: (visible: boolean) => {
        if (visible) binding.markNeedsCommit(host);
      },
    };
    this.#handlers = handlers;
    Object.assign(viewport, handlers);
  }

  build(_context: BuildContext): View {
    const cfg = this.#renderView.configuration;
    const widget = this.view.widget;
    const content = new MediaQuery({
      data: new MediaQueryData({
        size: cfg.size, devicePixelRatio: cfg.devicePixelRatio, windowFocused: this.#focused,
        windowControls: toRect(widget?.controls ?? null),
      }),
      // 各窗口的焦点子树在焦点树根下并列(不是 view 树里外层窗口的后代): 默认快捷键的节点挂在根下, 窗口的
      // 作用域是它的子节点, 按键因此从作用域冒泡到默认快捷键。
      child: new Shortcuts({
        debugLabel: "RootView Shortcuts",
        shortcuts: this.#shortcuts,
        parentNode: FocusManager.instance.rootScope,
        child: new Actions({
          actions: this.#actions,
          child: new FocusScope({
            node: this.#scope,
            autofocus: true,  // 还没有任何窗口持有焦点时取得焦点
            child: new Overlay({ initialEntries: [this.#entry!] }),
          }),
        }),
      }),
    });
    return new RawView(this.#renderView, new TickerMode({
      enabled: this.#open,
      child: widget !== null ? new WidgetScope({ widget, child: content }) : content,
    }));
  }
}

// ---------------------------------------------------------------------------
// 入口

/**
 * 启动应用: 以 view 为根构建 view 树并请求首帧。根不绑定任何窗口, 每个窗口的内容由树里的 RootView 渲染——
 * 启动时宿主创建的窗口是 Widget.fromId(0)(同 Flutter 的 runWidget + View), 所有窗口对等。任何窗口都可以关闭;
 * 最后一个窗口关闭后应用照常运行(app 'window-all-closed'), 是否退出由应用决定。根 view 本身不应产生渲染对象
 * (通常是 RootView 或 ViewAnchor({ views: [...] }), 可包在 MaterialApp 等只提供主题的 view 里)。
 */
export function runView(view: View): ViewsBinding {
  const binding = ViewsBinding.ensureInitialized();
  binding.attachRootView(view);
  binding.scheduleWarmUpFrame();
  return binding;
}

/**
 * 启动应用(单窗口的常用入口): 把 app 渲染进启动窗口, 即
 * runView(new RootView({ widget: Widget.fromId(0), child: app }))。其他窗口用 Widget.create 打开, 内容用
 * ViewAnchor + RootView 挂在树里。
 * 再次调用(替换 app)沿用第一次的窗口, 子树结构因此不随窗口开关而变化(否则挂在根下的其他窗口会被重建);
 * 启动窗口已关闭时 app 照常构建, 只是不渲染到任何窗口。
 */
export function runApp(app: View): ViewsBinding {
  const mounted = ViewsBinding.ensureInitialized().rootView;
  const reuse = mounted instanceof RootView && mounted.key === null
    && (mounted.widget === null || mounted.widget.id === kImplicitViewId);
  const widget = reuse ? mounted.widget : Widget.fromId(kImplicitViewId);
  return runView(new RootView({ widget, child: app }));
}
