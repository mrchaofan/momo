// 多视图(多窗口)组件(对标 Flutter 的 View / ViewAnchor)。一个 JS 运行时驱动多个原生窗口:
//   - widget/element 树只有一棵: 其他窗口的内容作为 ViewAnchor 的"旁支"(views)挂在主树里, 因此共享
//     JS 状态、主题等祖先 InheritedWidget, 也可以用 GlobalKey 把子树从一个窗口移到另一个窗口;
//   - 渲染树每个窗口一棵: View 为窗口建立自己的 RenderView 与 PipelineOwner, 每帧由 WidgetsBinding
//     与主窗口一起 layout/paint, 再提交到该窗口(momo:ui View.render);
//   - 每个窗口有自己的 MediaQuery(尺寸/dpr/焦点)、Overlay(对话框只出现在本窗口)与根焦点作用域
//     (挂在焦点树根下, 与主窗口并列;窗口获得系统焦点时恢复其上次的焦点)。
// Window 是声明式封装: 挂载即 createView 打开窗口, 卸载即关闭;用户点关闭按钮时先问 onCloseRequested,
// 关闭后回调 onClosed(应用据此从状态里移除该窗口)。
import { createView, views, type View as UiView } from "momo:ui";
import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { Size } from "../painting/geometry";
import type { RenderBox } from "../rendering/box";
import { PipelineOwner, type RenderObject } from "../rendering/object";
import { RenderView, type ViewConfiguration } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import { Actions, Shortcuts, defaultActions, defaultShortcuts } from "./actions";
import { WidgetsBinding, type ViewRenderHost } from "./binding";
import { FocusManager, FocusScopeNode } from "./focus_manager";
import { FocusScope } from "./focus_scope";
import {
  Element, RenderObjectElement, RenderObjectWidget, State, StatefulWidget, Widget, type BuildContext,
} from "./framework";
import { MediaQuery, MediaQueryData } from "./media_query";
import { Overlay, OverlayEntry } from "./overlay";
import { ViewScope } from "./view_scope";

// ---------------------------------------------------------------------------
// 渲染侧: 不挂到祖先渲染树的 RenderView 根

class RawView extends RenderObjectWidget {
  constructor(readonly renderView: RenderView, readonly child: Widget | null) {
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
    this.#child = this.updateChild(this.#child, (this.widget as RawView).child, null);
  }
  override update(newWidget: Widget): void {
    super.update(newWidget);
    this.#child = this.updateChild(this.#child, (this.widget as RawView).child, null);
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

/** 窗口尚未就绪时 views 中的占位: 不产生任何渲染对象。 */
class Nothing extends Widget {
  createElement(): Element {
    return new NothingElement(this);
  }
}
class NothingElement extends Element {
  protected performRebuild(): void {
    this.dirty = false;
  }
}
const kNothing = new Nothing();

/**
 * 在 child 所在位置正常渲染 child;views(View / Window)挂在 element 树的这里(继承此处的
 * InheritedWidget 与状态), 但渲染到各自的窗口。
 */
export class ViewAnchor extends Widget {
  readonly child: Widget | null;
  readonly views: readonly Widget[];
  constructor(o: { key?: Key | null; child?: Widget | null; views?: readonly Widget[] }) {
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
  override update(newWidget: Widget): void {
    super.update(newWidget);
    this.#rebuildChildren();
  }
  protected performRebuild(): void {
    this.dirty = false;
    this.#rebuildChildren();
  }
  #rebuildChildren(): void {
    const w = this.widget as ViewAnchor;
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
// View: 把 child 渲染进一个已打开的窗口

function configurationOf(view: UiView): ViewConfiguration {
  const dpr = view.devicePixelRatio || 1;
  return { size: new Size(view.physicalSize.width / dpr, view.physicalSize.height / dpr), devicePixelRatio: dpr };
}

/** 把 child 渲染进窗口 view(momo:ui View)。通常不直接使用, 见 {@link Window}。 */
export class View extends StatefulWidget {
  readonly view: UiView;
  readonly child: Widget;
  readonly onCloseRequested: (() => boolean | void) | null;
  constructor(o: { key?: Key | null; view: UiView; child: Widget; onCloseRequested?: (() => boolean | void) | null }) {
    super(o.key);
    this.view = o.view;
    this.child = o.child;
    this.onCloseRequested = o.onCloseRequested ?? null;
  }
  createState(): State {
    return new ViewState();
  }
}

class ViewState extends State<View> {
  #host!: ViewRenderHost;
  #entry: OverlayEntry | null = null;
  #scope = new FocusScopeNode({ debugLabel: "View Scope" });
  readonly #shortcuts = defaultShortcuts();
  readonly #actions = defaultActions();
  #focused = false;

  override initState(): void {
    const binding = WidgetsBinding.instance;
    const scheduler = SchedulerBinding.instance;
    const pipelineOwner = new PipelineOwner(() => scheduler.ensureVisualUpdate());
    const renderView = new RenderView(configurationOf(this.widget.view));
    pipelineOwner.rootNode = renderView;
    renderView.prepareInitialFrame();
    this.#host = { view: this.widget.view, renderView, pipelineOwner };
    binding.registerView(this.#host);
    this.#entry = new OverlayEntry({ builder: () => this.widget.child, opaque: true });
    this.#focused = this.widget.view.focused;
    this.#bind(this.widget.view);
  }

  override didUpdateWidget(old: View): void {
    if (old.view !== this.widget.view) {
      throw new Error("View: the view of a mounted View cannot change (use a different key)");
    }
    this.#entry?.markNeedsBuild();
  }

  override dispose(): void {
    const view = this.widget.view;
    for (const k of ["onMetricsChanged", "onPointerPacket", "onKeyData", "onFocusChanged", "onCloseRequested"] as const) {
      (view as unknown as Record<string, unknown>)[k] = null;
    }
    WidgetsBinding.instance.unregisterView(this.#host);
    this.#host.pipelineOwner.rootNode = null;
    this.#scope.dispose();
  }

  #bind(view: UiView): void {
    const gestures = WidgetsBinding.instance.gestures;
    view.onMetricsChanged = () => {
      this.#host.renderView.configuration = configurationOf(view);
      if (this.mounted) this.setState();
    };
    view.onPointerPacket = (data: Float64Array, count: number) => {
      try {
        gestures.handlePointerPacket(data, count, this.#host.renderView.configuration.devicePixelRatio, view.id);
      } catch (e) {
        reportError(e, "while handling a pointer packet");
      }
    };
    view.onKeyData = (data) => {
      try {
        return HardwareKeyboard.instance.handleKeyData(data);
      } catch (e) {
        reportError(e, "while handling key data");
        return false;
      }
    };
    view.onFocusChanged = (focused: boolean) => {
      if (!focused) HardwareKeyboard.instance.clearState();
      // 窗口获得系统焦点: 把 momo 焦点移到本窗口(恢复其上次的焦点)。
      if (focused && !this.#scope.hasFocus) this.#scope.requestFocus();
      if (this.mounted) this.setState(() => (this.#focused = focused));
    };
    view.onCloseRequested = () => this.widget.onCloseRequested?.();
  }

  build(_context: BuildContext): Widget {
    const cfg = this.#host.renderView.configuration;
    return new RawView(this.#host.renderView, new ViewScope({ view: this.widget.view, child: new MediaQuery({
      data: new MediaQueryData({ size: cfg.size, devicePixelRatio: cfg.devicePixelRatio, windowFocused: this.#focused }),
      child: new Shortcuts({
        debugLabel: "View Shortcuts",
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

// ---------------------------------------------------------------------------
// Window: 声明式地打开/关闭一个窗口

/** Window 参数(尺寸为逻辑像素)。 */
export interface WindowOptions {
  key?: Key | null;
  title?: string;
  width: number;
  height: number;
  minWidth?: number;
  minHeight?: number;
  /** 置顶: 窗口浮在普通窗口之上。 */
  alwaysOnTop?: boolean;
  /** 初始位置: 内容区左上角的屏幕坐标(逻辑点, 主屏左上原点), x 与 y 同时给出时生效;缺省居中。 */
  x?: number;
  y?: number;
  /**
   * 面板(同 Electron 的 type: 'panel'): 可接收键盘(key), 但不成为 main window, 聚焦它也不激活应用。
   * 适合浮动的辅助窗口。仅在打开时生效。缺省 false。
   */
  panel?: boolean;
  /** 用户能否拖动边框缩放窗口(打开时的初值;之后由 WindowChrome 的 resizable 修改)。缺省 true。 */
  resizable?: boolean;
  /** 用户点了窗口的关闭按钮: 返回 false 取消关闭。 */
  onCloseRequested?: (() => boolean | void) | null;
  /** 窗口已打开(child 将在下一帧渲染进去)。适合在此把已有子树(GlobalKey)移进新窗口。 */
  onOpened?: ((view: UiView) => void) | null;
  /** 窗口已被关闭(用户关闭, 或宿主关闭): 应用应据此把该窗口从状态中移除。 */
  onClosed?: (() => void) | null;
  /** 打开失败(宿主不支持多窗口等)。缺省报告错误。 */
  onCreateError?: ((error: unknown) => void) | null;
  child: Widget;
}

/**
 * 一个原生窗口。放在 {@link ViewAnchor} 的 views 中: 挂载时打开窗口, 卸载时关闭;child 渲染在窗口里,
 * 与主窗口共享 JS 状态。title 变化时更新窗口标题。
 */
export class Window extends StatefulWidget {
  readonly opts: WindowOptions;
  constructor(o: WindowOptions) {
    super(o.key);
    this.opts = o;
  }
  createState(): State {
    return new WindowState();
  }
}

export class WindowState extends State<Window> {
  #view: UiView | null = null;
  #closedByHost = false;
  #disposed = false;

  /** 已打开的窗口(打开中为 null)。 */
  get view(): UiView | null {
    return this.#view;
  }

  override initState(): void {
    const o = this.widget.opts;
    WidgetsBinding.instance.addViewsListener(this.#onViewsChanged);
    createView({ title: o.title, width: o.width, height: o.height, minWidth: o.minWidth, minHeight: o.minHeight, x: o.x, y: o.y, panel: o.panel, resizable: o.resizable }).then(
      (view) => {
        if (this.#disposed) {
          view.close();  // 打开期间已被移除
          return;
        }
        if (this.widget.opts.title !== undefined) view.title = this.widget.opts.title;
        if (this.widget.opts.alwaysOnTop) view.setAlwaysOnTop(true);
        this.setState(() => (this.#view = view));
        this.widget.opts.onOpened?.(view);
      },
      (e: unknown) => {
        if (this.#disposed) return;
        const onError = this.widget.opts.onCreateError;
        if (onError) onError(e);
        else reportError(e, "opening a window");
      },
    );
  }

  override didUpdateWidget(old: Window): void {
    const title = this.widget.opts.title;
    if (this.#view !== null && title !== undefined && title !== old.opts.title) this.#view.title = title;
    const top = this.widget.opts.alwaysOnTop ?? false;
    if (this.#view !== null && top !== (old.opts.alwaysOnTop ?? false)) this.#view.setAlwaysOnTop(top);
  }

  /** 把窗口带到最前。 */
  activate(): void {
    this.#view?.activate();
  }

  override dispose(): void {
    this.#disposed = true;
    WidgetsBinding.instance.removeViewsListener(this.#onViewsChanged);
    if (this.#view !== null && !this.#closedByHost && views.has(this.#view.id)) this.#view.close();
  }

  #onViewsChanged = (): void => {
    const view = this.#view;
    if (view === null || this.#closedByHost || views.has(view.id)) return;
    // 窗口被用户(或宿主)关闭。
    this.#closedByHost = true;
    this.setState(() => (this.#view = null));
    this.widget.opts.onClosed?.();
  };

  build(_context: BuildContext): Widget {
    const view = this.#view;
    if (view === null) return kNothing;
    return new View({ view, child: this.widget.opts.child, onCloseRequested: () => this.widget.opts.onCloseRequested?.() });
  }
}
