// 标签模型与标签栏。
// 插入: 宽度 0 → 目标宽度、透明度 0 → 1(easeOutCubic);删除: 反向(easeInCubic), 动画结束后才从列表移除。
// 标签内容始终按完整宽度排版, 外层按动画进度裁剪, 因此缓动过程中文字不会被挤压重排。
// 拖拽(类似 Chromium): 标签栏只识别拖动的开始, 应用随即捕获指针, 之后的移动与松开直接交给应用的拖动会话;
// 应用据此决定在哪个窗口的标签栏里排序、何时拖出成窗口、何时并入另一个窗口(见 main.ts DragSession)。
// 排序时被拖的标签原位置留空, 浮动副本跟随指针, 其他标签平移让位。
import {
  AnimatedBuilder, AnimationController, Border, BorderRadius, BorderSide, GlobalKey, BoxDecoration, ClipRect, CurvedAnimation, Curves,
  EdgeInsets, Expanded, GestureDetector, IgnorePointer, ImplicitlyAnimatedView, ImplicitlyAnimatedViewState, MouseRegion,
  Offset, Opacity, Padding, Positioned, Row, SizedBox, Stack, State, StatefulView, Text, Transform, Tween, Container, Center, ValueKey,
  type BuildContext, type DragDownDetails, type DragStartDetails, type Key, type TickerProvider, type View,
} from "@momo/framework";
import { WindowDragArea } from "./window_drag_area";
import type { WebViewController, WebViewState } from "@momo-examples/ext-webview";
import type { BrowserWindow, DetachedWindow } from "./main";
import { Spinner } from "./icons";
import { Palette, TextStyles, kTabAnimationMs, kTabHeight, kTabStripHeight, kTabStripTopInset } from "./style";

let nextTabId = 1;

/** 一个标签: 页面状态(来自 WebView)+ 插入/删除动画。 */
export class Tab {
  readonly id = nextTabId++;
  /** 标签页内容(WebView)的 GlobalKey: 标签移到另一个窗口时网页对象随之迁移, 不重新加载。 */
  readonly pageKey = new GlobalKey();
  controller: WebViewController | null = null;
  url = "";
  title = "";
  loading = false;
  progress = 0;
  canGoBack = false;
  canGoForward = false;
  /** 正在播放删除动画(已不可交互, 动画结束后移除)。 */
  closing = false;
  /** 已弹出到单独窗口(标签仍属于原窗口, 原处显示占位)。 */
  detached: DetachedWindow | null = null;
  /**
   * 拖出成新窗口、新窗口尚在打开时: 网页暂由这个(原)窗口继续渲染(隐藏), 新窗口打开后同一帧移过去,
   * GlobalKey 子树始终挂在树上, 页面不刷新。
   */
  parking: BrowserWindow | null = null;
  readonly animation: AnimationController;
  readonly curve: CurvedAnimation;

  /** vsync: 应用级 State(而非某个窗口的页面), 标签在窗口之间移动后动画仍有效。 */
  constructor(vsync: TickerProvider, readonly initialUrl: string | null) {
    this.url = initialUrl ?? "";
    this.animation = new AnimationController({ vsync, duration: kTabAnimationMs });
    this.curve = new CurvedAnimation({ parent: this.animation, curve: Curves.easeOutCubic, reverseCurve: Curves.easeInCubic });
  }

  get displayTitle(): string {
    if (this.title !== "") return this.title;
    if (this.url === "" || this.url.startsWith("momo://newtab")) return "新标签页";
    return this.url.replace(/^[a-z]+:\/\//i, "").replace(/\/$/, "");
  }

  apply(s: WebViewState): void {
    this.url = s.url;
    this.title = s.title;
    this.loading = s.loading;
    this.progress = s.progress;
    this.canGoBack = s.canGoBack;
    this.canGoForward = s.canGoForward;
  }

  dispose(): void {
    this.animation.dispose();
  }
}

// ---------------------------------------------------------------------------

/** 小号可点击文字图标(×、+), 悬停时出现浅灰底。 */
export class IconTap extends StatefulView {
  constructor(readonly o: { glyph: string; onTap: (() => void) | null; size?: number; style?: typeof TextStyles.icon }) {
    super();
  }
  createState(): State {
    return new IconTapState();
  }
}

class IconTapState extends State<IconTap> {
  #hover = false;
  build(_c: BuildContext): View {
    const { glyph, onTap, size = 28, style } = this.view.o;
    const enabled = onTap !== null;
    return new MouseRegion({
      cursor: enabled ? "click" : "basic",
      onEnter: () => this.setState(() => (this.#hover = true)),
      onExit: () => this.setState(() => (this.#hover = false)),
      child: new GestureDetector({
        behavior: "opaque",
        onTap,
        child: new Container({
          width: size,
          height: size,
          decoration: new BoxDecoration({ color: this.#hover && enabled ? Palette.hover : Palette.white, borderRadius: BorderRadius.circular(size / 2) }),
          child: new Center({ child: new Text(glyph, { style: enabled ? (style ?? TextStyles.icon) : TextStyles.iconDisabled }) }),
        }),
      }),
    });
  }
}

// ---------------------------------------------------------------------------

interface TabItemOptions {
  tab: Tab;
  active: boolean;
  width: number;
  onSelect: () => void;
  onClose: () => void;
  /** 右键: position 为窗口逻辑坐标。 */
  onContextMenu: (position: Offset) => void;
  /** 拖动中: 原位置留空(浮动副本画在上方)。 */
  placeholder?: boolean;
}

/** 单个标签: 宽度与透明度跟随动画;激活时下方一条黑线。 */
class TabItem extends StatefulView {
  constructor(readonly o: TabItemOptions) {
    super(new ValueKey(o.tab.id));
  }
  createState(): State {
    return new TabItemState();
  }
}

class TabItemState extends State<TabItem> {
  #hover = false;

  build(_c: BuildContext): View {
    const { tab, active, width, onSelect, onClose, onContextMenu, placeholder = false } = this.view.o;
    const showClose = !tab.closing && (active || this.#hover);
    const face = new MouseRegion({
      cursor: "click",
      onEnter: () => this.setState(() => (this.#hover = true)),
      onExit: () => this.setState(() => (this.#hover = false)),
      child: new GestureDetector({
        behavior: "opaque",
        onTap: tab.closing ? null : onSelect,
        onSecondaryTapUp: tab.closing ? null : (d) => onContextMenu(d.globalPosition),
        child: new Container({
          decoration: new BoxDecoration({
            color: this.#hover && !active ? Palette.hover : Palette.white,
            border: new Border({
              bottom: new BorderSide({ color: active ? Palette.black : Palette.line, width: active ? 2 : 1 }),
              right: new BorderSide({ color: Palette.line, width: 1 }),
            }),
          }),
          padding: EdgeInsets.only({ left: 14, right: 6 }),
          child: new Row({
            children: [
              ...(tab.loading ? [new Spinner({ size: 12 }), new SizedBox({ width: 7 })] : []),
              new Expanded({
                child: new Text(tab.displayTitle, { style: active ? TextStyles.tabActive : TextStyles.tab, maxLines: 1, overflow: "ellipsis" }),
              }),
              new SizedBox({ width: 22, height: 22, child: showClose ? new IconTap({ glyph: "×", size: 22, style: TextStyles.close, onTap: onClose }) : null }),
            ],
          }),
        }),
      }),
    });
    // 内容按完整宽度排版, 外层宽度随动画变化并裁剪。
    return new AnimatedBuilder({
      animation: tab.curve,
      child: face,
      builder: (_context, child) => {
        const t = tab.curve.value;
        return new SizedBox({
          width: width * t,
          height: kTabHeight,
          child: new ClipRect({
            child: new Opacity({
              opacity: placeholder ? 0 : t,
              child: new Stack({ children: [new Positioned({ left: 0, top: 0, width, height: kTabHeight, child: child! })] }),
            }),
          }),
        });
      },
    });
  }
}

// ---------------------------------------------------------------------------

/** 标签宽度: 按窗口宽度均分, 限制在 96–220。 */
export function tabWidthFor(windowWidth: number, count: number): number {
  return Math.max(96, Math.min(220, (windowWidth - 48) / Math.max(1, count)));
}

/**
 * 拖动中标签的左边界限制在标签区内(第一个到最后一个标签位)。leading: 标签区左侧的留白——系统窗口控制按钮
 * 占用的宽度(MediaQuery.windowControls, 由宿主报告)。
 */
export function clampDragLeft(left: number, tabWidth: number, count: number, leading: number): number {
  return Math.max(leading, Math.min(leading + (count - 1) * tabWidth, left));
}

/** 被拖标签左边界为 left 时的落点下标(count 为未关闭的标签数)。 */
export function dropIndex(left: number, tabWidth: number, count: number, leading: number): number {
  const slot = Math.round((clampDragLeft(left, tabWidth, count, leading) - leading) / tabWidth);
  return Math.max(0, Math.min(count - 1, slot));
}

/** 水平平移, 目标变化时缓动过去;instant 时直接到位(排序提交那一帧, 避免回弹)。 */
class AnimatedShift extends ImplicitlyAnimatedView {
  constructor(readonly o: { key: Key; dx: number; instant: boolean; child: View }) {
    super({ key: o.key, duration: 150, curve: Curves.easeOutCubic });
  }
  createState(): State {
    return new AnimatedShiftState();
  }
}

class AnimatedShiftState extends ImplicitlyAnimatedViewState<AnimatedShift> {
  readonly #dx = new Tween<number>(0, 0);
  protected forEachTween(visitor: (tween: Tween<number>, target: number) => void): void {
    visitor(this.#dx, this.view.o.dx);
  }
  override didUpdateView(old: AnimatedShift): void {
    if (this.view.o.instant) {
      this.#dx.begin = this.view.o.dx;
      this.#dx.end = this.view.o.dx;
    }
    super.didUpdateView(old);
  }
  build(): View {
    const dx = this.#dx.transform(this.animation.value);
    return dx === 0 ? this.view.o.child : Transform.translate({ offset: new Offset(dx, 0), child: this.view.o.child });
  }
}

/** 本窗口标签栏里正在排序的标签: left 为它左边界的窗口逻辑 x。 */
export interface TabDrag {
  tab: Tab;
  left: number;
}

export interface TabStripOptions {
  tabs: readonly Tab[];
  activeId: number;
  tabWidth: number;
  /** 标签区左侧的留白(系统窗口控制按钮占用的宽度)。 */
  leading: number;
  /** 正在本标签栏里排序的标签(拖出成窗口或在别的窗口时为 null)。 */
  drag: TabDrag | null;
  onSelect: (tab: Tab) => void;
  onClose: (tab: Tab) => void;
  onContextMenu: (tab: Tab, position: Offset) => void;
  onNew: () => void;
  /**
   * 开始拖动标签: grab 为按下点相对标签左上角的偏移, screen 为指针屏幕位置。接收方随即捕获指针
   * (GestureBinding.capturePointer), 之后的移动与松开不再经过标签栏。
   */
  onDragStart: (tab: Tab, grab: Offset, screen: Offset) => void;
}

/** 标签栏: 标签 + 新建按钮, 底部一条细线;处理标签拖动。 */
export class TabStrip extends StatefulView {
  constructor(readonly o: TabStripOptions) {
    super();
  }
  createState(): State {
    return new TabStripState();
  }
}

class TabStripState extends State<TabStrip> {
  /** 按下位置(标签栏内;拖动开始时据此找到被按下的标签)。 */
  #downLocal: Offset | null = null;
  /** 本帧刚结束排序: 让位平移直接归零(标签顺序已提交, 不再回弹)。 */
  #instant = false;

  override didUpdateView(old: TabStrip): void {
    super.didUpdateView(old);
    this.#instant = old.o.drag !== null && this.view.o.drag === null;
  }

  /** 标签栏内 (x, y) 处的标签(按动画中的实际宽度累计)及其左边界;顶部缩放边上不算。 */
  #tabAt(x: number, y: number): { tab: Tab; left: number } | null {
    if (y < kTabStripTopInset) return null;
    const { tabs, tabWidth, leading } = this.view.o;
    let left = leading;
    for (const tab of tabs) {
      const w = tabWidth * tab.curve.value;
      if (x >= left && x < left + w) return tab.closing ? null : { tab, left };
      left += w;
    }
    return null;
  }

  #onPanDown = (d: DragDownDetails): void => {
    this.#downLocal = d.localPosition;
  };

  #onPanStart = (d: DragStartDetails): void => {
    const down = this.#downLocal;
    const hit = down !== null ? this.#tabAt(down.dx, down.dy) : null;
    if (hit === null) return;
    // 抓取点相对标签左上角(标签在顶部缩放边之下);位置用屏幕坐标, 拖到窗口外也成立。
    this.view.o.onDragStart(hit.tab, new Offset(down!.dx - hit.left, down!.dy - kTabStripTopInset), d.screenPosition);
  };

  build(_c: BuildContext): View {
    const { tabs, activeId, tabWidth, leading, drag, onSelect, onClose, onContextMenu, onNew } = this.view.o;
    const instant = this.#instant;
    this.#instant = false;
    const live = tabs.filter((t) => !t.closing);
    const from = drag !== null ? live.indexOf(drag.tab) : -1;
    const to = from >= 0 ? dropIndex(drag!.left, tabWidth, live.length, leading) : -1;
    /** 其他标签的让位平移: 被拖标签从 from 移到 to, 中间的标签向反方向挪一格。 */
    const shiftOf = (tab: Tab): number => {
      if (from < 0 || tab.closing) return 0;
      const i = live.indexOf(tab);
      if (from < to && i > from && i <= to) return -tabWidth;
      if (to < from && i >= to && i < from) return tabWidth;
      return 0;
    };
    const item = (tab: Tab, placeholder: boolean): View => new TabItem({
      tab,
      active: tab.id === activeId,
      width: tabWidth,
      placeholder,
      onSelect: () => onSelect(tab),
      onClose: () => onClose(tab),
      onContextMenu: (p) => onContextMenu(tab, p),
    });
    const row = new Row({
      children: [
        // 系统窗口控制按钮所在的空白: 也可拖动窗口。
        new SizedBox({ width: leading, height: kTabHeight, child: new WindowDragArea() }),
        ...tabs.map((tab) => new AnimatedShift({
          key: new ValueKey(tab.id),
          dx: shiftOf(tab),
          instant,
          child: item(tab, from >= 0 && tab === drag!.tab),
        })),
        new Padding({ padding: EdgeInsets.symmetric({ horizontal: 6 }), child: new IconTap({ glyph: "+", onTap: onNew }) }),
        // 标签之后的空白: 拖动窗口, 双击缩放。
        new Expanded({ child: new WindowDragArea() }),
      ],
    });
    // 浮动副本: 跟随指针, 限制在标签区内。
    const floating = from >= 0
      ? [new Positioned({
          left: clampDragLeft(drag!.left, tabWidth, live.length, leading),
          top: 0,
          width: tabWidth,
          height: kTabHeight,
          child: new IgnorePointer({ child: new TabItem({ tab: drag!.tab, active: true, width: tabWidth, onSelect: () => {}, onClose: () => {}, onContextMenu: () => {} }) }),
        })]
      : [];
    return new GestureDetector({
      onPanDown: this.#onPanDown,
      onPanStart: this.#onPanStart,
      child: new Container({
        height: kTabStripHeight,
        decoration: new BoxDecoration({ color: Palette.white, border: new Border({ bottom: new BorderSide({ color: Palette.line, width: 1 }) }) }),
        // 顶部缩放边: 留空(不是拖动区域), 窗口顶边缩放由系统原生处理。
        padding: EdgeInsets.only({ top: kTabStripTopInset }),
        child: new Stack({ children: [row, ...floating] }),
      }),
    });
  }
}
