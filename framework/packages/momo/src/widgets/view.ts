// 多视图(多窗口)组件(对标 Flutter 的 View / ViewAnchor)。一个 JS 运行时驱动多个原生窗口:
//   - widget/element 树只有一棵: 其他窗口的内容作为 ViewAnchor 的"旁支"(views)挂在主树里, 因此共享
//     JS 状态、主题等祖先 InheritedWidget, 也可以用 GlobalKey 把子树从一个窗口移到另一个窗口;
//   - 渲染树每个窗口一棵: View 为窗口建立自己的 RenderView 与 PipelineOwner, 每帧由 WidgetsBinding
//     与主窗口一起 layout/paint, 再提交到该窗口(momo:ui View.render);
//   - 每个窗口有自己的 MediaQuery(尺寸/dpr/焦点)、Overlay(对话框只出现在本窗口)与根焦点作用域
//     (挂在焦点树根下, 与主窗口并列;窗口获得系统焦点时恢复其上次的焦点)。
// Window 是声明式封装: 挂载即打开窗口(或接管已有视图, 如宿主启动时创建的视图 0), 卸载即关闭;窗口的全部属性
// (标题、尺寸限制、置顶、可缩放…)都在 Window 上声明, 随 widget 更新;用户点关闭按钮时先问 onCloseRequested,
// 关闭后回调 onClosed(应用据此从状态里移除该窗口)。窗口操作经 WindowService(宿主的 momo/window 通道)。
import { views, type View as UiView } from "momo:ui";
import { reportError } from "../foundation/assert";
import type { Key } from "../foundation/key";
import { Rect, Size } from "../painting/geometry";
import type { RenderBox } from "../rendering/box";
import { PipelineOwner, type RenderObject } from "../rendering/object";
import { RenderView, type ViewConfiguration } from "../rendering/view";
import { SchedulerBinding } from "../scheduler/binding";
import { HardwareKeyboard } from "../services/hardware_keyboard";
import { WindowService } from "../services/window";
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
  readonly #onWindowChange = (view: number): void => {
    if (view === this.widget.view.id && this.mounted) this.setState();
  };

  override initState(): void {
    const binding = WidgetsBinding.instance;
    WindowService.instance.addListener(this.#onWindowChange);
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
    for (const k of ["onMetricsChanged", "onPointerPacket", "onKeyData", "onFocusChanged"] as const) {
      (view as unknown as Record<string, unknown>)[k] = null;
    }
    WindowService.instance.setCloseRequestHandler(view.id, null);
    WindowService.instance.removeListener(this.#onWindowChange);
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
    WindowService.instance.setCloseRequestHandler(view.id, () => this.widget.onCloseRequested?.());
  }

  build(_context: BuildContext): Widget {
    const cfg = this.#host.renderView.configuration;
    const controls = WindowService.instance.controlsOf(this.widget.view.id);
    return new RawView(this.#host.renderView, new ViewScope({ view: this.widget.view, child: new MediaQuery({
      data: new MediaQueryData({
        size: cfg.size, devicePixelRatio: cfg.devicePixelRatio, windowFocused: this.#focused,
        windowControls: controls !== null ? Rect.fromLTWH(controls.x, controls.y, controls.width, controls.height) : null,
      }),
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

/**
 * Window 参数(逻辑像素)。一个窗口的全部属性都在这里声明:
 *   - 打开时生效: width/height(新开窗口时必需)、x/y、nonActivating;
 *   - 随 widget 更新: title、minWidth/minHeight、alwaysOnTop、resizable。
 * 外观(隐藏标题栏、缩放边框)由窗口内容里的 WindowChrome 声明。
 */
export interface WindowOptions {
  key?: Key | null;
  /**
   * 接管一个已打开的视图(而不是新开窗口), 如宿主启动时创建的视图 0: Window({ view: views.get(0), ... })。
   * 接管后与新开的窗口没有区别: 属性随 widget 更新, 卸载即关闭。
   */
  view?: UiView | null;
  title?: string;
  width?: number;
  height?: number;
  minWidth?: number;
  minHeight?: number;
  /** 置顶: 窗口浮在普通窗口之上。 */
  alwaysOnTop?: boolean;
  /** 用户能否拖动边框缩放窗口。缺省 true。 */
  resizable?: boolean;
  /** 初始位置: 内容区左上角的屏幕坐标(逻辑点, 主屏左上原点), x 与 y 同时给出时生效;缺省居中。 */
  x?: number;
  y?: number;
  /**
   * 辅助窗口: 可接收键盘, 但不成为应用的主窗口, 聚焦它也不激活应用(同 Electron 的 type: 'panel')。
   * 适合浮动的辅助窗口。仅在新开窗口时生效。缺省 false。
   */
  nonActivating?: boolean;
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
 * 一个原生窗口。放在 {@link ViewAnchor} 的 views 中(或作为 runWidget 的窗口): 挂载时打开窗口(或接管 view),
 * 卸载时关闭;child 渲染在窗口里, 与其他窗口共享 JS 状态。
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
    if (o.view != null) {
      // 接管已有视图: 立即可用;onOpened 推迟到构建之外(回调里通常会 setState)。
      const view = o.view;
      this.#view = view;
      this.#applyProperties(view.id, null);
      void Promise.resolve().then(() => {
        if (!this.#disposed && this.#view === view) this.widget.opts.onOpened?.(view);
      });
      return;
    }
    const windows = WindowService.instance;
    windows.create({
      title: o.title, width: o.width ?? 800, height: o.height ?? 600, minWidth: o.minWidth, minHeight: o.minHeight,
      x: o.x, y: o.y, nonActivating: o.nonActivating, resizable: o.resizable,
    }).then(
      (id) => {
        const view = views.get(id);
        if (this.#disposed) {
          windows.close(id);  // 打开期间已被移除
          return;
        }
        if (view === undefined) {
          this.#fail(new Error("Window: the host closed the window before it opened"));
          return;
        }
        // 打开期间属性可能已变化: 以当前 widget 为准补齐(创建时已带 title/resizable)。
        this.#applyProperties(id, { ...o, alwaysOnTop: false });
        this.setState(() => (this.#view = view));
        this.widget.opts.onOpened?.(view);
      },
      (e: unknown) => this.#fail(e),
    );
  }

  #fail(e: unknown): void {
    if (this.#disposed) return;
    const onError = this.widget.opts.onCreateError;
    if (onError) onError(e);
    else reportError(e, "opening a window");
  }

  /** 把与 old 不同的可更新属性交给宿主(old 为 null: 全部)。 */
  #applyProperties(id: number, old: WindowOptions | null): void {
    const o = this.widget.opts;
    const windows = WindowService.instance;
    if (o.title !== undefined && o.title !== old?.title) windows.setTitle(id, o.title);
    if ((o.minWidth !== undefined || o.minHeight !== undefined) &&
        (o.minWidth !== old?.minWidth || o.minHeight !== old?.minHeight)) {
      windows.setMinSize(id, o.minWidth ?? 0, o.minHeight ?? 0);
    }
    const top = o.alwaysOnTop ?? false;
    if (top !== (old?.alwaysOnTop ?? false) || (old === null && top)) windows.setAlwaysOnTop(id, top);
    const resizable = o.resizable ?? true;
    if (old === null ? o.resizable !== undefined : resizable !== (old.resizable ?? true)) {
      windows.setResizable(id, resizable);
    }
  }

  override didUpdateWidget(old: Window): void {
    if (this.#view !== null) this.#applyProperties(this.#view.id, old.opts);
  }

  /** 把窗口带到最前。 */
  activate(): void {
    if (this.#view !== null) WindowService.instance.activate(this.#view.id);
  }

  override dispose(): void {
    this.#disposed = true;
    WidgetsBinding.instance.removeViewsListener(this.#onViewsChanged);
    if (this.#view !== null && !this.#closedByHost && views.has(this.#view.id)) WindowService.instance.close(this.#view.id);
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
