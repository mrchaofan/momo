import type { TextAlign } from "momo:ui";
import type { Key } from "../foundation/key";
import type { TextDirection } from "../painting/geometry";
import type { TextOverflow, TextWidthBasis } from "../painting/text_painter";
import { TextSpan } from "../painting/text_span";
import { TextStyle } from "../painting/text_style";
import { RenderParagraph } from "../rendering/paragraph";
import { InheritedWidget, LeafRenderObjectWidget, StatelessWidget, type BuildContext, type Widget } from "./framework";
import { MediaQuery } from "./media_query";

/** 子树默认文本样式。 */
export class DefaultTextStyle extends InheritedWidget {
  readonly style: TextStyle;
  readonly textAlign: TextAlign | null;
  readonly softWrap: boolean;
  readonly overflow: TextOverflow;
  readonly maxLines: number | null;

  constructor(o: { key?: Key | null; style: TextStyle; textAlign?: TextAlign | null; softWrap?: boolean; overflow?: TextOverflow; maxLines?: number | null; child: Widget }) {
    super(o);
    this.style = o.style;
    this.textAlign = o.textAlign ?? null;
    this.softWrap = o.softWrap ?? true;
    this.overflow = o.overflow ?? "clip";
    this.maxLines = o.maxLines ?? null;
  }

  /** 与外层默认样式合并。 */
  static merge(o: { key?: Key | null; style?: TextStyle | null; textAlign?: TextAlign | null; maxLines?: number | null; overflow?: TextOverflow; child: Widget; context: BuildContext }): DefaultTextStyle {
    const parent = DefaultTextStyle.of(o.context);
    return new DefaultTextStyle({
      key: o.key ?? null,
      style: parent.style.merge(o.style ?? null),
      textAlign: o.textAlign ?? parent.textAlign,
      softWrap: parent.softWrap,
      overflow: o.overflow ?? parent.overflow,
      maxLines: o.maxLines ?? parent.maxLines,
      child: o.child,
    });
  }

  static readonly fallback = new TextStyle({ fontSize: 14, color: undefined });

  /** 最近的默认样式(无则回退)。 */
  static of(context: BuildContext): DefaultTextStyle {
    return context.dependOnInheritedWidgetOfExactType(DefaultTextStyle) ?? FALLBACK;
  }

  updateShouldNotify(old: DefaultTextStyle): boolean {
    return !old.style.equals(this.style) || old.textAlign !== this.textAlign || old.softWrap !== this.softWrap ||
      old.overflow !== this.overflow || old.maxLines !== this.maxLines;
  }
}

class FallbackChild extends StatelessWidget {
  build(): Widget {
    return this;
  }
}
const FALLBACK = new DefaultTextStyle({ style: new TextStyle({ fontSize: 14 }), child: new FallbackChild() });

/** 富文本(直接给 TextSpan, 不合并默认样式)。 */
export class RichText extends LeafRenderObjectWidget {
  readonly text: TextSpan;
  readonly textAlign: TextAlign;
  readonly textDirection: TextDirection;
  readonly softWrap: boolean;
  readonly overflow: TextOverflow;
  readonly maxLines: number | null;
  readonly textScaleFactor: number;
  readonly textWidthBasis: TextWidthBasis;

  constructor(o: { key?: Key | null; text: TextSpan; textAlign?: TextAlign; textDirection?: TextDirection; softWrap?: boolean; overflow?: TextOverflow; maxLines?: number | null; textScaleFactor?: number; textWidthBasis?: TextWidthBasis }) {
    super(o.key);
    this.text = o.text;
    this.textAlign = o.textAlign ?? "start";
    this.textDirection = o.textDirection ?? "ltr";
    this.softWrap = o.softWrap ?? true;
    this.overflow = o.overflow ?? "clip";
    this.maxLines = o.maxLines ?? null;
    this.textScaleFactor = o.textScaleFactor ?? 1;
    this.textWidthBasis = o.textWidthBasis ?? "parent";
  }
  createRenderObject(): RenderParagraph {
    return new RenderParagraph(this.text, {
      textAlign: this.textAlign, textDirection: this.textDirection, softWrap: this.softWrap, overflow: this.overflow,
      maxLines: this.maxLines, textScaleFactor: this.textScaleFactor, textWidthBasis: this.textWidthBasis,
    });
  }
  override updateRenderObject(_c: BuildContext, r: RenderParagraph): void {
    r.text = this.text;
    r.textAlign = this.textAlign;
    r.textDirection = this.textDirection;
    r.softWrap = this.softWrap;
    r.overflow = this.overflow;
    r.maxLines = this.maxLines;
    r.textScaleFactor = this.textScaleFactor;
    r.textWidthBasis = this.textWidthBasis;
  }
}

/** Text 参数。 */
export interface TextOptions {
  key?: Key | null;
  style?: TextStyle | null;
  textAlign?: TextAlign | null;
  softWrap?: boolean | null;
  overflow?: TextOverflow | null;
  maxLines?: number | null;
  textScaleFactor?: number | null;
  textWidthBasis?: TextWidthBasis;
}

/** 单一样式文本(与 DefaultTextStyle 合并)。 */
export class Text extends StatelessWidget {
  readonly data: string | null;
  readonly textSpan: TextSpan | null;
  readonly style: TextStyle | null;
  readonly textAlign: TextAlign | null;
  readonly softWrap: boolean | null;
  readonly overflow: TextOverflow | null;
  readonly maxLines: number | null;
  readonly textScaleFactor: number | null;
  readonly textWidthBasis: TextWidthBasis;

  constructor(data: string, o: TextOptions = {}) {
    super(o.key);
    this.data = data;
    this.textSpan = null;
    this.style = o.style ?? null;
    this.textAlign = o.textAlign ?? null;
    this.softWrap = o.softWrap ?? null;
    this.overflow = o.overflow ?? null;
    this.maxLines = o.maxLines ?? null;
    this.textScaleFactor = o.textScaleFactor ?? null;
    this.textWidthBasis = o.textWidthBasis ?? "parent";
  }

  /** 富文本版本。 */
  static rich(span: TextSpan, o: TextOptions = {}): Text {
    const t = new Text("", o);
    (t as { textSpan: TextSpan | null }).textSpan = span;
    (t as { data: string | null }).data = null;
    return t;
  }

  build(context: BuildContext): Widget {
    const d = DefaultTextStyle.of(context);
    let effective = d.style;
    if (this.style !== null) effective = effective.merge(this.style);
    const children = this.textSpan !== null ? [this.textSpan] : null;
    const scale = this.textScaleFactor ?? MediaQuery.maybeOf(context)?.textScaleFactor ?? 1;
    return new RichText({
      text: new TextSpan({ style: effective, text: this.data, children }),
      textAlign: this.textAlign ?? d.textAlign ?? "start",
      softWrap: this.softWrap ?? d.softWrap,
      overflow: this.overflow ?? d.overflow,
      maxLines: this.maxLines ?? d.maxLines,
      textScaleFactor: scale,
      textWidthBasis: this.textWidthBasis,
    });
  }
}
