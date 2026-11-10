import type { Key } from "../foundation/key";
import { EdgeInsets } from "../painting/edge_insets";
import { Colors } from "../painting/color";
import { Expanded, MouseRegion, Row, SizedBox, Column } from "../widgets/basic";
import { Container } from "../widgets/container";
import { State, StatefulWidget, type BuildContext, type Widget } from "../widgets/framework";
import { GestureDetector } from "../widgets/gesture_detector";
import { DefaultTextStyle } from "../widgets/text";
import { Theme } from "./theme";

/** 列表行:leading / title / subtitle / trailing, 可点击(hover 高亮)。 */
export class ListTile extends StatefulWidget {
  readonly leading: Widget | null;
  readonly title: Widget | null;
  readonly subtitle: Widget | null;
  readonly trailing: Widget | null;
  readonly onTap: (() => void) | null;
  readonly contentPadding: EdgeInsets;
  constructor(o: { key?: Key | null; leading?: Widget | null; title?: Widget | null; subtitle?: Widget | null; trailing?: Widget | null; onTap?: (() => void) | null; contentPadding?: EdgeInsets }) {
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
  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    const w = this.widget;
    const texts: Widget[] = [];
    if (w.title !== null) texts.push(new DefaultTextStyle({ style: theme.textTheme.bodyLarge, child: w.title }));
    if (w.subtitle !== null) texts.push(new DefaultTextStyle({ style: theme.textTheme.bodyMedium.copyWith({ color: theme.colorScheme.onSurface.withOpacity(0.6) }), child: w.subtitle }));
    const row: Widget[] = [];
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
