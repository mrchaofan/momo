import {
  ParagraphBuilder, type Canvas, type LineMetrics, type Paragraph, type ParagraphStyle, type TextAlign,
} from "momo:ui";
import { Offset, Rect, Size, type TextDirection } from "./geometry";
import { TextSpan } from "./text_span";
import { TextStyle } from "./text_style";

/** 文本位置(UTF-16 下标 + 亲和性)。 */
export interface TextPosition {
  offset: number;
  affinity: "downstream" | "upstream";
}

/** 文本溢出处理。 */
export type TextOverflow = "clip" | "ellipsis" | "visible";

/** 文本宽度的计算基准:parent = 按约束(多行填满), longestLine = 按最长行收缩。 */
export type TextWidthBasis = "parent" | "longestLine";

/** 段落 LRU 缓存:键 = (文本树, 段落样式, 排版宽度)。 */
class ParagraphCache {
  #map = new Map<string, Paragraph>();
  capacity = 512;
  hits = 0;
  misses = 0;
  get(key: string): Paragraph | undefined {
    const p = this.#map.get(key);
    if (p !== undefined) {
      this.#map.delete(key);
      this.#map.set(key, p);
      this.hits++;
    } else {
      this.misses++;
    }
    return p;
  }
  set(key: string, p: Paragraph): void {
    this.#map.set(key, p);
    while (this.#map.size > this.capacity) {
      const oldest = this.#map.keys().next().value;
      if (oldest === undefined) break;
      // 被逐出的 Paragraph 可能仍被某个 TextPainter 持有, 不主动 dispose, 交给 GC。
      this.#map.delete(oldest);
    }
  }
  get size(): number {
    return this.#map.size;
  }
  clear(): void {
    this.#map.clear();
    this.hits = 0;
    this.misses = 0;
  }
}

/** 全局段落缓存(可调 capacity / 查看命中率)。 */
export const paragraphCache = new ParagraphCache();

/**
 * 文本排版与绘制(包装 momo:ui ParagraphBuilder/Paragraph)。
 * 排版结果按(文本树 cacheKey, 段落样式, 宽度)缓存于 {@link paragraphCache}。
 */
export class TextPainter {
  #text: TextSpan | null;
  #textAlign: TextAlign;
  #textDirection: TextDirection;
  #maxLines: number | null;
  #ellipsis: string | null;
  #textScaleFactor: number;
  #strutHeight: number | null;
  #widthBasis: TextWidthBasis;
  #paragraph: Paragraph | null = null;
  #contentKey: string | null = null;
  #layoutWidth = -1;
  #width = 0;
  #minWidth = 0;
  #maxWidth = Infinity;
  #lineHeightPainter: TextPainter | null = null;

  constructor(o: {
    text?: TextSpan | null;
    textAlign?: TextAlign;
    textDirection?: TextDirection;
    maxLines?: number | null;
    ellipsis?: string | null;
    textScaleFactor?: number;
    strutHeight?: number | null;
    textWidthBasis?: TextWidthBasis;
  } = {}) {
    this.#text = o.text ?? null;
    this.#textAlign = o.textAlign ?? "start";
    this.#textDirection = o.textDirection ?? "ltr";
    this.#maxLines = o.maxLines ?? null;
    this.#ellipsis = o.ellipsis ?? null;
    this.#textScaleFactor = o.textScaleFactor ?? 1;
    this.#strutHeight = o.strutHeight ?? null;
    this.#widthBasis = o.textWidthBasis ?? "parent";
  }

  #invalidate(): void {
    this.#paragraph = null;
    this.#contentKey = null;
    this.#lineHeightPainter = null;
  }

  get text(): TextSpan | null {
    return this.#text;
  }
  set text(v: TextSpan | null) {
    if (v === this.#text || (v !== null && this.#text !== null && v.cacheKey === this.#text.cacheKey)) {
      this.#text = v;
      return;
    }
    this.#text = v;
    this.#invalidate();
  }
  get textAlign(): TextAlign {
    return this.#textAlign;
  }
  set textAlign(v: TextAlign) {
    if (v !== this.#textAlign) {
      this.#textAlign = v;
      this.#invalidate();
    }
  }
  get textDirection(): TextDirection {
    return this.#textDirection;
  }
  set textDirection(v: TextDirection) {
    if (v !== this.#textDirection) {
      this.#textDirection = v;
      this.#invalidate();
    }
  }
  get maxLines(): number | null {
    return this.#maxLines;
  }
  set maxLines(v: number | null) {
    if (v !== this.#maxLines) {
      this.#maxLines = v;
      this.#invalidate();
    }
  }
  get ellipsis(): string | null {
    return this.#ellipsis;
  }
  set ellipsis(v: string | null) {
    if (v !== this.#ellipsis) {
      this.#ellipsis = v;
      this.#invalidate();
    }
  }
  get textScaleFactor(): number {
    return this.#textScaleFactor;
  }
  set textScaleFactor(v: number) {
    if (v !== this.#textScaleFactor) {
      this.#textScaleFactor = v;
      this.#invalidate();
    }
  }
  get textWidthBasis(): TextWidthBasis {
    return this.#widthBasis;
  }
  set textWidthBasis(v: TextWidthBasis) {
    if (v !== this.#widthBasis) {
      this.#widthBasis = v;
      this.#layoutWidth = -1;
    }
  }

  /** 内容是否需要重新排版(文本或样式变更后为 true)。 */
  get needsLayout(): boolean {
    return this.#paragraph === null;
  }

  #paragraphStyle(): ParagraphStyle {
    const s: ParagraphStyle = { textAlign: this.#textAlign, textDirection: this.#textDirection };
    if (this.#maxLines !== null) s.maxLines = this.#maxLines;
    if (this.#ellipsis !== null) s.ellipsis = this.#ellipsis;
    if (this.#strutHeight !== null) s.height = this.#strutHeight;
    return s;
  }

  #computeContentKey(): string {
    if (this.#contentKey === null) {
      this.#contentKey = `${this.#textAlign}|${this.#textDirection}|${this.#maxLines}|${this.#ellipsis}|${this.#textScaleFactor}|${this.#strutHeight}|${this.#text?.cacheKey ?? ""}`;
    }
    return this.#contentKey;
  }

  #scaledSpan(): TextSpan {
    const span = this.#text ?? new TextSpan({ text: "" });
    if (this.#textScaleFactor === 1) return span;
    const scale = (s: TextSpan): TextSpan =>
      new TextSpan({
        text: s.text,
        style: s.style && s.style.fontSize !== undefined ? s.style.copyWith({ fontSize: s.style.fontSize * this.#textScaleFactor }) : s.style,
        children: s.children ? s.children.map(scale) : null,
      });
    return scale(span);
  }

  #obtainParagraph(width: number): Paragraph {
    const key = `${this.#computeContentKey()}@${width}`;
    let p = paragraphCache.get(key);
    if (p === undefined) {
      const builder = new ParagraphBuilder(this.#paragraphStyle());
      this.#scaledSpan().build(builder);
      p = builder.build();
      p.layout(width);
      paragraphCache.set(key, p);
    }
    return p;
  }

  /** 以 [minWidth, maxWidth] 排版;内容与约束都未变时为空操作。 */
  layout(minWidth = 0, maxWidth = Infinity): void {
    if (this.#paragraph !== null && minWidth === this.#minWidth && maxWidth === this.#maxWidth) return;
    this.#minWidth = minWidth;
    this.#maxWidth = maxWidth;
    let p = this.#obtainParagraph(maxWidth);
    const intrinsic = this.#widthBasis === "longestLine" ? p.longestLine : p.maxIntrinsicWidth;
    const width = Math.min(Math.max(Math.ceil(intrinsic), minWidth), maxWidth);
    const align = this.#textAlign;
    const needsAligned = align === "center" || align === "right" || align === "justify" ||
      (align === "end" && this.#textDirection === "ltr") || (align === "start" && this.#textDirection === "rtl");
    if (needsAligned && width !== maxWidth) p = this.#obtainParagraph(width);
    this.#paragraph = p;
    this.#layoutWidth = p.width;
    this.#width = width;
  }

  #ensure(): Paragraph {
    if (this.#paragraph === null) throw new Error("TextPainter.layout() must be called before use");
    return this.#paragraph;
  }

  /** 排版后的宽度(已按约束钳制)。 */
  get width(): number {
    this.#ensure();
    return this.#width;
  }
  get height(): number {
    return Math.ceil(this.#ensure().height);
  }
  get size(): Size {
    return new Size(this.width, this.height);
  }
  get minIntrinsicWidth(): number {
    return Math.ceil(this.#ensure().minIntrinsicWidth);
  }
  get maxIntrinsicWidth(): number {
    return Math.ceil(this.#ensure().maxIntrinsicWidth);
  }
  get didExceedMaxLines(): boolean {
    return this.#ensure().didExceedMaxLines;
  }
  /** 实际排版宽度(Paragraph.layout 的参数)。 */
  get layoutWidth(): number {
    return this.#layoutWidth;
  }
  /** 底层 Paragraph(高级用法)。 */
  get paragraph(): Paragraph {
    return this.#ensure();
  }

  /** 首行基线到顶部的距离。 */
  computeDistanceToActualBaseline(baseline: "alphabetic" | "ideographic" = "alphabetic"): number {
    const p = this.#ensure();
    return baseline === "alphabetic" ? p.alphabeticBaseline : p.ideographicBaseline;
  }

  /** 单行高度(以同样式的空白文本测量)。 */
  get preferredLineHeight(): number {
    if (this.#lineHeightPainter === null) {
      this.#lineHeightPainter = new TextPainter({
        text: new TextSpan({ text: " ", style: this.#firstStyle() }),
        textScaleFactor: this.#textScaleFactor,
        strutHeight: this.#strutHeight,
      });
      this.#lineHeightPainter.layout();
    }
    return this.#lineHeightPainter.height;
  }

  #firstStyle(): TextStyle | null {
    let found: TextStyle | null = null;
    this.#text?.visit((s) => {
      if (s.style !== null) {
        found = found === null ? s.style : found.merge(s.style);
        return false;
      }
      return true;
    });
    return found;
  }

  /** 在 offset 处绘制。 */
  paint(canvas: Canvas, offset: Offset): void {
    canvas.drawParagraph(this.#ensure(), offset.dx, offset.dy);
  }

  /** [start, end) 的选区矩形。 */
  getBoxesForSelection(start: number, end: number): Rect[] {
    const raw = this.#ensure().getBoxesForRange(start, end);
    const out: Rect[] = [];
    for (let i = 0; i + 4 < raw.length; i += 5) {
      out.push(new Rect(raw[i]!, raw[i + 1]!, raw[i + 2]!, raw[i + 3]!));
    }
    return out;
  }

  /** 点 → 文本位置。 */
  getPositionForOffset(offset: Offset): TextPosition {
    return this.#ensure().getPositionForOffset(offset.dx, offset.dy);
  }

  /** 单词边界 [start, end)。 */
  getWordBoundary(offset: number): { start: number; end: number } {
    return this.#ensure().getWordBoundary(offset);
  }

  /** 行度量。 */
  computeLineMetrics(): LineMetrics[] {
    return this.#ensure().getLineMetrics();
  }

  /** 光标(插入点 offset)左上角位置。 */
  getOffsetForCaret(offset: number, affinity: "downstream" | "upstream" = "downstream"): Offset {
    const p = this.#ensure();
    const text = this.#text?.toPlainText() ?? "";
    const len = text.length;
    const lineHeight = this.preferredLineHeight;
    // 上游:用前一个字符的右缘
    const prevBox = (): Offset | null => {
      if (offset <= 0) return null;
      const prev = text.charCodeAt(offset - 1);
      if (prev === 10) return null;
      const start = offset >= 2 && prev >= 0xdc00 && prev <= 0xdfff ? offset - 2 : offset - 1;
      const boxes = p.getBoxesForRange(start, offset);
      if (boxes.length < 5) return null;
      const n = boxes.length - 5;
      return new Offset(boxes[n + 2]!, boxes[n + 1]!);
    };
    const nextBox = (): Offset | null => {
      if (offset >= len) return null;
      const c = text.charCodeAt(offset);
      const end = c >= 0xd800 && c <= 0xdbff ? offset + 2 : offset + 1;
      const boxes = p.getBoxesForRange(offset, Math.min(end, len));
      if (boxes.length < 5) return null;
      return new Offset(boxes[0]!, boxes[1]!);
    };
    const r = affinity === "upstream" ? prevBox() ?? nextBox() : nextBox() ?? prevBox();
    if (r !== null) return r;
    // 空文本或行首为换行
    if (offset > 0 && text.charCodeAt(offset - 1) === 10) {
      // 换行后的空行:落在最后一行开头或下一行
      const metrics = p.getLineMetrics();
      for (const m of metrics) {
        if (m.startIndex === offset) return new Offset(this.#alignOffsetX(), m.baseline - m.ascent);
      }
      return new Offset(this.#alignOffsetX(), Math.max(0, p.height - lineHeight));
    }
    return new Offset(this.#alignOffsetX(), 0);
  }

  #alignOffsetX(): number {
    const a = this.#textAlign;
    const w = this.#width;
    if (a === "center") return w / 2;
    if (a === "right" || (a === "end" && this.#textDirection === "ltr")) return w;
    return 0;
  }

  /** 光标高度(单行高)。 */
  getFullHeightForCaret(): number {
    return this.preferredLineHeight;
  }
}
