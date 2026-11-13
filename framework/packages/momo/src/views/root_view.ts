// 多窗口(对标 Chromium views::internal::RootView / Flutter 的 View 与 ViewAnchor)。一个 JS 运行时驱动多个原生窗口:
//   - view/element 树只有一棵: 其他窗口的内容作为 ViewAnchor 的"旁支"(views)挂在主树里, 因此共享
//     JS 状态、主题等祖先 InheritedView, 也可以用 GlobalKey 把子树从一个窗口移到另一个窗口;
//   - 渲染树每个窗口一棵: RootView 为窗口建立自己的 RenderView 与 PipelineOwner, 每帧由 ViewsBinding
//     与主窗口一起 layout/paint, 再提交到该窗口的视图(momo:ui Viewport.render);
//   - 每个窗口有自己的 MediaQuery(尺寸/dpr/焦点)、Overlay(对话框只出现在本窗口)与根焦点作用域
//     (挂在焦点树根下, 与主窗口并列;窗口获得系统焦点时恢复其上次的焦点)。
// 窗口本身(打开、关闭、标题、状态…)是 Widget 对象(services/widget.ts): RootView 只负责把内容渲染进去。
//   const win = await Widget.create({ width: 800, height: 600 });
//   ... ViewAnchor({ child, views: [new RootView({ key, widget: win, child: new Editor() })] })
import type { Viewport } from "momo:ui";
import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { Rect, Size } from "../painting/geometry";
import type { RenderBox } from "../rendering/box";
import { PipelineOwner, type RenderObject } from "../rendering/object";
import { RenderView, type ViewConfiguration } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import type { Widget, WidgetBounds } from "../services/widget";
import { Actions, Shortcuts, defaultActions, defaultShortcuts } from "./actions";
import { ViewsBinding, type ViewRenderHost } from "./binding";
import { FocusManager, FocusScopeNode } from "./focus_manager";
import { FocusScope } from "./focus_scope";
import {
  Element, RenderObjectElement, RenderObjectView, State, StatefulView, View, type BuildContext,
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

function configurationOf(viewport: Viewport): ViewConfiguration {
  const dpr = viewport.devicePixelRatio || 1;
  return { size: new Size(viewport.physicalSize.width / dpr, viewport.physicalSize.height / dpr), devicePixelRatio: dpr };
}

function toRect(b: WidgetBounds | null): Rect | null {
  return b !== null ? Rect.fromLTWH(b.x, b.y, b.width, b.height) : null;
}

/**
 * 把 child 渲染进窗口 widget。放在 {@link ViewAnchor} 的 views 中(或作为 runView 的根下的旁支)。
 * RootView 不打开也不关闭窗口: 窗口关闭('closed')后应用把它从树里移除。
 */
export class RootView extends StatefulView {
  readonly widget: Widget;
  readonly child: View;
  constructor(o: { key?: Key | null; widget: Widget; child: View }) {
    super(o.key);
    this.widget = o.widget;
    this.child = o.child;
  }
  createState(): State {
    return new RootViewState();
  }
}

class RootViewState extends State<RootView> {
  #host!: ViewRenderHost;
  #viewport!: Viewport;
  #entry: OverlayEntry | null = null;
  #scope = new FocusScopeNode({ debugLabel: "RootView Scope" });
  readonly #shortcuts = defaultShortcuts();
  readonly #actions = defaultActions();
  #focused = false;
  readonly #onControlsChanged = (): void => {
    if (this.mounted) this.setState();
  };

  override initState(): void {
    const widget = this.view.widget;
    const viewport = widget.viewport;
    if (viewport === null) throw new Error(`RootView: window ${widget.id} is already closed`);
    this.#viewport = viewport;
    const scheduler = SchedulerBinding.instance;
    const pipelineOwner = new PipelineOwner(() => scheduler.ensureVisualUpdate());
    const renderView = new RenderView(configurationOf(viewport));
    pipelineOwner.rootNode = renderView;
    renderView.prepareInitialFrame();
    this.#host = { viewport, renderView, pipelineOwner };
    ViewsBinding.instance.registerView(this.#host);
    this.#entry = new OverlayEntry({ builder: () => this.view.child, opaque: true });
    this.#focused = viewport.focused;
    widget.on("controls-changed", this.#onControlsChanged);
    widget.on("state-changed", this.#onControlsChanged);
    this.#bind(viewport);
  }

  override didUpdateView(old: RootView): void {
    if (old.widget !== this.view.widget) {
      throw new Error("RootView: the widget of a mounted RootView cannot change (use a different key)");
    }
    this.#entry?.markNeedsBuild();
  }

  override dispose(): void {
    const viewport = this.#viewport;
    for (const k of ["onMetricsChanged", "onPointerPacket", "onKeyData", "onFocusChanged"] as const) {
      (viewport as unknown as Record<string, unknown>)[k] = null;
    }
    this.view.widget.off("controls-changed", this.#onControlsChanged);
    this.view.widget.off("state-changed", this.#onControlsChanged);
    ViewsBinding.instance.unregisterView(this.#host);
    this.#host.pipelineOwner.rootNode = null;
    this.#scope.dispose();
  }

  #bind(viewport: Viewport): void {
    const gestures = ViewsBinding.instance.gestures;
    viewport.onMetricsChanged = () => {
      this.#host.renderView.configuration = configurationOf(viewport);
      if (this.mounted) this.setState();
    };
    viewport.onPointerPacket = (data: Float64Array, count: number) => {
      try {
        gestures.handlePointerPacket(data, count, this.#host.renderView.configuration.devicePixelRatio, viewport.id);
      } catch (e) {
        reportError(e, "while handling a pointer packet");
      }
    };
    viewport.onKeyData = (data) => {
      try {
        return HardwareKeyboard.instance.handleKeyData(data);
      } catch (e) {
        reportError(e, "while handling key data");
        return false;
      }
    };
    viewport.onFocusChanged = (focused: boolean) => {
      if (!focused) HardwareKeyboard.instance.clearState();
      // 窗口获得系统焦点: 把 momo 焦点移到本窗口(恢复其上次的焦点)。
      if (focused && !this.#scope.hasFocus) this.#scope.requestFocus();
      if (this.mounted) this.setState(() => (this.#focused = focused));
    };
  }

  build(_context: BuildContext): View {
    const cfg = this.#host.renderView.configuration;
    const widget = this.view.widget;
    return new RawView(this.#host.renderView, new WidgetScope({ widget, child: new MediaQuery({
      data: new MediaQueryData({
        size: cfg.size, devicePixelRatio: cfg.devicePixelRatio, windowFocused: this.#focused,
        windowControls: toRect(widget.controls),
      }),
      child: new Shortcuts({
        debugLabel: "RootView Shortcuts",
        shortcuts: this.#shortcuts,
        child: new Actions({
          actions: this.#actions,
          child: new FocusScope({
            node: this.#scope,
            parentNode: FocusManager.instance.rootScope,
            child: new Overlay({ initialEntries: [this.#entry!] }),
          }),
        }),
      }),
    }) }));
  }
}
