// 标签模型与标签栏。
// 插入: 宽度 0 → 目标宽度、透明度 0 → 1(easeOutCubic);删除: 反向(easeInCubic), 动画结束后才从列表移除。
// 标签内容始终按完整宽度排版, 外层按动画进度裁剪, 因此缓动过程中文字不会被挤压重排。
import {
  AnimatedBuilder, AnimationController, Border, BorderRadius, BorderSide, GlobalKey, BoxDecoration, ClipRect, CurvedAnimation, Curves,
  EdgeInsets, Expanded, GestureDetector, MouseRegion, Opacity, Padding, Positioned, Row, SizedBox, Stack, State,
  StatefulWidget, StatelessWidget, Text, Container, Center, ValueKey, WindowDragArea,
  type BuildContext, type Offset, type TickerProvider, type Widget,
} from "@momo/framework";
import type { WebViewController, WebViewState } from "@momo-examples/ext-webview";
import type { DetachedWindow } from "./main";
import { Palette, TextStyles, kTabAnimationMs, kTabStripHeight, kTrafficLightsWidth } from "./style";

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
export class IconTap extends StatefulWidget {
  constructor(readonly o: { glyph: string; onTap: (() => void) | null; size?: number; style?: typeof TextStyles.icon }) {
    super();
  }
  createState(): State {
    return new IconTapState();
  }
}

class IconTapState extends State<IconTap> {
  #hover = false;
  build(_c: BuildContext): Widget {
    const { glyph, onTap, size = 28, style } = this.widget.o;
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
}

/** 单个标签: 宽度与透明度跟随动画;激活时下方一条黑线。 */
class TabItem extends StatefulWidget {
  constructor(readonly o: TabItemOptions) {
    super(new ValueKey(o.tab.id));
  }
  createState(): State {
    return new TabItemState();
  }
}

class TabItemState extends State<TabItem> {
  #hover = false;

  build(_c: BuildContext): Widget {
    const { tab, active, width, onSelect, onClose, onContextMenu } = this.widget.o;
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
              ...(tab.loading ? [new Text("◌ ", { style: TextStyles.tab })] : []),
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
          height: kTabStripHeight,
          child: new ClipRect({
            child: new Opacity({
              opacity: t,
              child: new Stack({ children: [new Positioned({ left: 0, top: 0, width, height: kTabStripHeight, child: child! })] }),
            }),
          }),
        });
      },
    });
  }
}

// ---------------------------------------------------------------------------

export interface TabStripOptions {
  tabs: readonly Tab[];
  activeId: number;
  tabWidth: number;
  onSelect: (tab: Tab) => void;
  onClose: (tab: Tab) => void;
  onContextMenu: (tab: Tab, position: Offset) => void;
  onNew: () => void;
}

/** 标签栏: 标签 + 新建按钮, 底部一条细线。 */
export class TabStrip extends StatelessWidget {
  constructor(readonly o: TabStripOptions) {
    super();
  }
  build(_c: BuildContext): Widget {
    const { tabs, activeId, tabWidth, onSelect, onClose, onContextMenu, onNew } = this.o;
    return new Container({
      height: kTabStripHeight,
      decoration: new BoxDecoration({ color: Palette.white, border: new Border({ bottom: new BorderSide({ color: Palette.line, width: 1 }) }) }),
      child: new Row({
        children: [
          // 红绿灯所在的空白: 也可拖动窗口。
          new SizedBox({ width: kTrafficLightsWidth, height: kTabStripHeight, child: new WindowDragArea() }),
          ...tabs.map((tab) => new TabItem({ tab, active: tab.id === activeId, width: tabWidth, onSelect: () => onSelect(tab), onClose: () => onClose(tab), onContextMenu: (p) => onContextMenu(tab, p) })),
          new Padding({ padding: EdgeInsets.symmetric({ horizontal: 6 }), child: new IconTap({ glyph: "+", onTap: onNew }) }),
          // 标签之后的空白: 拖动窗口, 双击缩放。
          new Expanded({ child: new WindowDragArea() }),
        ],
      }),
    });
  }
}
