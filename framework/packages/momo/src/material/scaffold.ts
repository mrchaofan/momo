import type { Key } from "../foundation/key";
import { Color } from "../painting/color";
import { BoxDecoration, BoxShadow } from "../painting/decoration";
import { EdgeInsets } from "../painting/edge_insets";
import { Offset } from "../painting/geometry";
import { Column, Expanded, Positioned, Row, SizedBox, Stack, Padding } from "../widgets/basic";
import { Container } from "../widgets/container";
import { StatelessWidget, type BuildContext, type Widget } from "../widgets/framework";
import { DefaultTextStyle } from "../widgets/text";
import { Theme } from "./theme";
import { BorderRadius } from "../painting/borders";

/** 顶部应用栏。 */
export class AppBar extends StatelessWidget {
  readonly title: Widget | null;
  readonly actions: readonly Widget[];
  readonly backgroundColor: Color | null;
  readonly foregroundColor: Color | null;
  readonly height: number;
  constructor(o: { key?: Key | null; title?: Widget | null; actions?: readonly Widget[]; backgroundColor?: Color | null; foregroundColor?: Color | null; height?: number } = {}) {
    super(o.key);
    this.title = o.title ?? null;
    this.actions = o.actions ?? [];
    this.backgroundColor = o.backgroundColor ?? null;
    this.foregroundColor = o.foregroundColor ?? null;
    this.height = o.height ?? 56;
  }
  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    const bg = this.backgroundColor ?? theme.colorScheme.primary;
    const fg = this.foregroundColor ?? theme.colorScheme.onPrimary;
    return new Container({
      height: this.height,
      padding: EdgeInsets.symmetric({ horizontal: 16 }),
      decoration: new BoxDecoration({ color: bg, boxShadow: [new BoxShadow({ color: new Color(0x33000000), offset: new Offset(0, 1), blurRadius: 3 })] }),
      child: new DefaultTextStyle({
        style: theme.textTheme.titleLarge.copyWith({ color: fg }),
        child: new Row({
          children: [
            ...(this.title !== null ? [new Expanded({ child: this.title })] : [new Expanded({ child: new SizedBox() })]),
            ...this.actions,
          ],
        }),
      }),
    });
  }
}

/** 页面骨架(简化版):appBar + body + floatingActionButton。 */
export class Scaffold extends StatelessWidget {
  readonly appBar: Widget | null;
  readonly body: Widget | null;
  readonly floatingActionButton: Widget | null;
  readonly backgroundColor: Color | null;
  constructor(o: { key?: Key | null; appBar?: Widget | null; body?: Widget | null; floatingActionButton?: Widget | null; backgroundColor?: Color | null } = {}) {
    super(o.key);
    this.appBar = o.appBar ?? null;
    this.body = o.body ?? null;
    this.floatingActionButton = o.floatingActionButton ?? null;
    this.backgroundColor = o.backgroundColor ?? null;
  }
  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    const column = new Column({
      crossAxisAlignment: "stretch",
      children: [
        ...(this.appBar !== null ? [this.appBar] : []),
        new Expanded({ child: this.body ?? new SizedBox() }),
      ],
    });
    return new Container({
      color: this.backgroundColor ?? theme.scaffoldBackgroundColor,
      child: new Stack({
        fit: "expand",
        children: [
          column,
          ...(this.floatingActionButton !== null ? [new Positioned({ right: 16, bottom: 16, child: this.floatingActionButton })] : []),
        ],
      }),
    });
  }
}

/** 分隔线。 */
export class Divider extends StatelessWidget {
  readonly height: number;
  readonly thickness: number;
  readonly color: Color | null;
  readonly indent: number;
  constructor(o: { key?: Key | null; height?: number; thickness?: number; color?: Color | null; indent?: number } = {}) {
    super(o.key);
    this.height = o.height ?? 16;
    this.thickness = o.thickness ?? 1;
    this.color = o.color ?? null;
    this.indent = o.indent ?? 0;
  }
  build(context: BuildContext): Widget {
    const color = this.color ?? Theme.of(context).dividerColor;
    return new SizedBox({
      height: this.height,
      child: new Padding({
        padding: EdgeInsets.only({ left: this.indent, top: (this.height - this.thickness) / 2, bottom: (this.height - this.thickness) / 2 }),
        child: new Container({ color }),
      }),
    });
  }
}

/** 卡片。 */
export class Card extends StatelessWidget {
  readonly child: Widget | null;
  readonly margin: EdgeInsets;
  readonly color: Color | null;
  constructor(o: { key?: Key | null; child?: Widget | null; margin?: EdgeInsets; color?: Color | null } = {}) {
    super(o.key);
    this.child = o.child ?? null;
    this.margin = o.margin ?? EdgeInsets.all(4);
    this.color = o.color ?? null;
  }
  build(context: BuildContext): Widget {
    const theme = Theme.of(context);
    return new Container({
      margin: this.margin,
      decoration: new BoxDecoration({
        color: this.color ?? theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(12),
        boxShadow: [new BoxShadow({ color: new Color(0x26000000), offset: new Offset(0, 1), blurRadius: 3 })],
      }),
      child: this.child,
    });
  }
}
