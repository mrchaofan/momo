// 窗口内上下文菜单(momo 绘制, 放在 Overlay 中)。
// 菜单与半透明遮罩都画在网页(平台视图)之上: 合成时进入网页上方的 overlay 图层, 遮罩压暗网页、
// 菜单盖住网页的一部分;派发器据此把这些区域的点击交给 momo(点遮罩关闭菜单, 网页收不到)。
import {
  AnimatedBuilder, AnimationController, Border, BorderSide, BorderRadius, BoxDecoration, BoxShadow, Color, ColoredBox,
  Column, Container, CurvedAnimation, Curves, EdgeInsets, Expanded, Focus, GestureDetector, KeyDownEvent,
  LogicalKeyboardKey, MediaQuery, MouseRegion, Offset, Opacity, OverlayEntry, Overlay, Positioned, Row, SizedBox, Stack,
  State, StatefulView, Text, Transform, Matrix4,
  type BuildContext, type View,
} from "@momo/framework";
import { Palette, TextStyles } from "./style";

export interface MenuItem {
  label: string;
  /** 右侧的提示字符(可选), 如 "↗"。 */
  hint?: string;
  /** null = 不可用(灰显)。 */
  onSelect: (() => void) | null;
}

const kMenuWidth = 232;
const kDim = new Color(0x1a000000);  // 遮罩: 约 10% 黑, 压暗网页

/** 在 position(窗口逻辑坐标)处弹出菜单。点菜单项、点遮罩或按 Esc 关闭。 */
export function showContextMenu(context: BuildContext, position: Offset, o: { title: string; items: MenuItem[] }): void {
  const overlay = Overlay.of(context);
  let entry: OverlayEntry | null = null;
  entry = new OverlayEntry({
    builder: () => new ContextMenu({ position, title: o.title, items: o.items, onClosed: () => entry?.remove() }),
  });
  overlay.insert(entry);
}

class ContextMenu extends StatefulView {
  constructor(readonly o: { position: Offset; title: string; items: MenuItem[]; onClosed: () => void }) {
    super();
  }
  createState(): State {
    return new ContextMenuState();
  }
}

class ContextMenuState extends State<ContextMenu> {
  #controller!: AnimationController;
  #curve!: CurvedAnimation;
  #closing = false;

  override initState(): void {
    super.initState();
    this.#controller = new AnimationController({ vsync: this, duration: 140, reverseDuration: 90 });
    this.#curve = new CurvedAnimation({ parent: this.#controller, curve: Curves.easeOutCubic, reverseCurve: Curves.easeInCubic });
    this.#controller.forward();
  }

  override dispose(): void {
    this.#controller.dispose();
    super.dispose();
  }

  /** 淡出后移除;action 在动画开始前执行(打开窗口等不必等动画)。 */
  #close = (action?: () => void): void => {
    if (this.#closing) return;
    this.#closing = true;
    action?.();
    void this.#controller.reverse().then(() => this.view.o.onClosed());
  };

  build(context: BuildContext): View {
    const { position, title, items } = this.view.o;
    const size = MediaQuery.of(context).size;
    const left = Math.max(8, Math.min(position.dx, size.width - kMenuWidth - 8));
    const top = position.dy + 6;

    const menu = new Container({
      width: kMenuWidth,
      padding: EdgeInsets.symmetric({ vertical: 6 }),
      decoration: new BoxDecoration({
        color: Palette.white,
        border: new Border({
          top: new BorderSide({ color: Palette.line }), right: new BorderSide({ color: Palette.line }),
          bottom: new BorderSide({ color: Palette.line }), left: new BorderSide({ color: Palette.line }),
        }),
        borderRadius: BorderRadius.circular(8),
        boxShadow: [new BoxShadow({ color: new Color(0x2e000000), offset: new Offset(0, 10), blurRadius: 28 })],
      }),
      child: new Column({
        mainAxisSize: "min",
        children: [
          new Container({
            padding: EdgeInsets.only({ left: 14, right: 14, top: 4, bottom: 8 }),
            child: new Text(title, { style: TextStyles.status, maxLines: 1, overflow: "ellipsis" }),
          }),
          new Container({ height: 1, color: Palette.line }),
          new SizedBox({ height: 4 }),
          ...items.map((item) => new MenuRow({ item, onTap: () => this.#close(item.onSelect ?? undefined) })),
        ],
      }),
    });

    return new Focus({
      autofocus: true,
      onKeyEvent: (_node, event) => {
        if (event instanceof KeyDownEvent && event.logicalKey === LogicalKeyboardKey.escape) {
          this.#close();
          return "handled";
        }
        return "ignored";
      },
      child: new AnimatedBuilder({
        animation: this.#curve,
        builder: (_c, _child) => {
          const t = this.#curve.value;
          return new Stack({
            children: [
              // 遮罩: 铺满窗口(包括网页区域), 点击任意处关闭。
              Positioned.fill({
                child: new GestureDetector({
                  behavior: "opaque",
                  onTap: () => this.#close(),
                  onSecondaryTap: () => this.#close(),
                  child: new Opacity({ opacity: t, child: new ColoredBox({ color: kDim }) }),
                }),
              }),
              // 菜单: 自上方轻微滑入并淡入。
              new Positioned({
                left,
                top,
                child: new Opacity({
                  opacity: t,
                  child: new Transform({ transform: Matrix4.translationValues(0, (1 - t) * -6, 0), child: menu }),
                }),
              }),
            ],
          });
        },
      }),
    });
  }
}

class MenuRow extends StatefulView {
  constructor(readonly o: { item: MenuItem; onTap: () => void }) {
    super();
  }
  createState(): State {
    return new MenuRowState();
  }
}

class MenuRowState extends State<MenuRow> {
  #hover = false;
  build(_c: BuildContext): View {
    const { item, onTap } = this.view.o;
    const enabled = item.onSelect !== null;
    return new MouseRegion({
      cursor: enabled ? "click" : "basic",
      onEnter: () => this.setState(() => (this.#hover = true)),
      onExit: () => this.setState(() => (this.#hover = false)),
      child: new GestureDetector({
        behavior: "opaque",
        onTap: enabled ? onTap : null,
        child: new Container({
          height: 32,
          margin: EdgeInsets.symmetric({ horizontal: 6 }),
          padding: EdgeInsets.symmetric({ horizontal: 10 }),
          decoration: new BoxDecoration({ color: this.#hover && enabled ? Palette.hover : Palette.white, borderRadius: BorderRadius.circular(5) }),
          child: new Row({
            children: [
              new Expanded({ child: new Text(item.label, { style: enabled ? TextStyles.menu : TextStyles.menuDisabled }) }),
              ...(item.hint ? [new Text(item.hint, { style: enabled ? TextStyles.menuHint : TextStyles.menuDisabled })] : []),
            ],
          }),
        }),
      }),
    });
  }
}
