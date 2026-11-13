import type { Key } from "../foundation/key";
import { EdgeInsets } from "../painting/edge_insets";
import { Colors } from "../painting/color";
import { Expanded, MouseRegion, Row, SizedBox, Column } from "../views/basic";
import { Container } from "../views/container";
import { State, StatefulView, type BuildContext, type View } from "../views/framework";
import { GestureDetector } from "../views/gesture_detector";
import { DefaultTextStyle } from "../views/text";
import { Theme } from "./theme";

/** 列表行:leading / title / subtitle / trailing, 可点击(hover 高亮)。 */
export class ListTile extends StatefulView {
  readonly leading: View | null;
  readonly title: View | null;
  readonly subtitle: View | null;
  readonly trailing: View | null;
  readonly onTap: (() => void) | null;
  readonly contentPadding: EdgeInsets;
  constructor(o: { key?: Key | null; leading?: View | null; title?: View | null; subtitle?: View | null; trailing?: View | null; onTap?: (() => void) | null; contentPadding?: EdgeInsets }) {
    super(o.key);
    this.leading = o.leading ?? null;
    this.title = o.title ?? null;
    this.subtitle = o.subtitle ?? null;
    this.trailing = o.trailing ?? null;
    this.onTap = o.onTap ?? null;
    this.contentPadding = o.contentPadding ?? EdgeInsets.symmetric({ horizontal: 16, vertical: 4 });
  }
  createState(): State {
    return new ListTileState();
  }
}

class ListTileState extends State<ListTile> {
  #hovered = false;
  build(context: BuildContext): View {
    const theme = Theme.of(context);
    const w = this.view;
    const texts: View[] = [];
    if (w.title !== null) texts.push(new DefaultTextStyle({ style: theme.textTheme.bodyLarge, child: w.title }));
    if (w.subtitle !== null) texts.push(new DefaultTextStyle({ style: theme.textTheme.bodyMedium.copyWith({ color: theme.colorScheme.onSurface.withOpacity(0.6) }), child: w.subtitle }));
    const row: View[] = [];
    if (w.leading !== null) row.push(w.leading, new SizedBox({ width: 16 }));
    row.push(new Expanded({ child: new Column({ mainAxisSize: "min", crossAxisAlignment: "start", children: texts }) }));
    if (w.trailing !== null) row.push(new SizedBox({ width: 16 }), w.trailing);
    const content = new Container({
      padding: w.contentPadding,
      color: this.#hovered && w.onTap !== null ? theme.colorScheme.onSurface.withOpacity(theme.hoverOpacity) : Colors.transparent,
      child: new Row({ children: row }),
    });
    return new MouseRegion({
      cursor: w.onTap !== null ? "click" : "defer",
      opaque: false,
      onEnter: () => this.setState(() => (this.#hovered = true)),
      onExit: () => this.setState(() => (this.#hovered = false)),
      child: w.onTap !== null ? new GestureDetector({ behavior: "opaque", onTap: w.onTap, child: content }) : content,
    });
  }
}
