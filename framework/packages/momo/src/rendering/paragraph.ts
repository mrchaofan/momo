import type { TextAlign } from "momo:ui";
import { Offset, Rect, Size, type TextDirection } from "../painting/geometry";
import { TextPainter, type TextOverflow, type TextPosition, type TextWidthBasis } from "../painting/text_painter";
import { TextSpan } from "../painting/text_span";
import { RenderBox, type TextBaseline } from "./box";
import { BoxConstraints } from "./box_constraints";
import type { PaintingContext } from "./object";

const kEllipsis = "…";

/** 富文本段落渲染。 */
export class RenderParagraph extends RenderBox {
  readonly #painter: TextPainter;
  #softWrap: boolean;
  #overflow: TextOverflow;
  #needsClipping = false;

  constructor(text: TextSpan, o: {
    textAlign?: TextAlign;
    textDirection?: TextDirection;
    softWrap?: boolean;
    overflow?: TextOverflow;
    maxLines?: number | null;
    textScaleFactor?: number;
    textWidthBasis?: TextWidthBasis;
  } = {}) {
    super();
    this.#softWrap = o.softWrap ?? true;
    this.#overflow = o.overflow ?? "clip";
    this.#painter = new TextPainter({
      text,
      textAlign: o.textAlign ?? "start",
      textDirection: o.textDirection ?? "ltr",
      maxLines: o.maxLines ?? null,
      ellipsis: this.#overflow === "ellipsis" ? kEllipsis : null,
      textScaleFactor: o.textScaleFactor ?? 1,
      textWidthBasis: o.textWidthBasis ?? "parent",
    });
  }

  /** 内部 TextPainter(已排版)。 */
  get textPainter(): TextPainter {
    return this.#painter;
  }

  get text(): TextSpan {
    return this.#painter.text!;
  }
  set text(v: TextSpan) {
    const old = this.#painter.text;
    if (old !== null && old.cacheKey === v.cacheKey) {
      this.#painter.text = v;
      return;
    }
    this.#painter.text = v;
    this.markNeedsLayout();
  }
  set textAlign(v: TextAlign) {
    if (v !== this.#painter.textAlign) {
      this.#painter.textAlign = v;
      this.markNeedsLayout();
    }
  }
  set textDirection(v: TextDirection) {
    if (v !== this.#painter.textDirection) {
      this.#painter.textDirection = v;
      this.markNeedsLayout();
    }
  }
  set softWrap(v: boolean) {
    if (v !== this.#softWrap) {
      this.#softWrap = v;
      this.markNeedsLayout();
    }
  }
  set overflow(v: TextOverflow) {
    if (v !== this.#overflow) {
      this.#overflow = v;
      this.#painter.ellipsis = v === "ellipsis" ? kEllipsis : null;
      this.markNeedsLayout();
    }
  }
  set maxLines(v: number | null) {
    if (v !== this.#painter.maxLines) {
      this.#painter.maxLines = v;
      this.markNeedsLayout();
    }
  }
  set textScaleFactor(v: number) {
    if (v !== this.#painter.textScaleFactor) {
      this.#painter.textScaleFactor = v;
      this.markNeedsLayout();
    }
  }
  set textWidthBasis(v: TextWidthBasis) {
    if (v !== this.#painter.textWidthBasis) {
      this.#painter.textWidthBasis = v;
      this.markNeedsLayout();
    }
  }

  #layoutText(minWidth: number, maxWidth: number): void {
    const wrap = this.#softWrap || this.#overflow === "ellipsis";
    this.#painter.layout(minWidth, wrap ? maxWidth : Infinity);
  }

  protected override computeMinIntrinsicWidth(_height: number): number {
    this.#layoutText(0, Infinity);
    return this.#painter.minIntrinsicWidth;
  }
  protected override computeMaxIntrinsicWidth(_height: number): number {
    this.#layoutText(0, Infinity);
    return this.#painter.maxIntrinsicWidth;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    this.#layoutText(width, width);
    return this.#painter.height;
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    return this.computeMinIntrinsicHeight(width);
  }
  protected override computeDistanceToActualBaseline(baseline: TextBaseline): number | null {
    this.#layoutText(this.constraints.minWidth, this.constraints.maxWidth);
    return this.#painter.computeDistanceToActualBaseline(baseline);
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    this.#layoutText(constraints.minWidth, constraints.maxWidth);
    return constraints.constrain(this.#painter.size);
  }

  override performLayout(): void {
    const c = this.constraints;
    this.#layoutText(c.minWidth, c.maxWidth);
    const textSize = this.#painter.size;
    this.size = c.constrain(textSize);
    const overflowW = this.size.width < textSize.width;
    const overflowH = this.size.height < textSize.height || this.#painter.didExceedMaxLines;
    this.#needsClipping = (overflowW || overflowH) && this.#overflow !== "visible";
  }

  override hitTestSelf(): boolean {
    return true;
  }

  override paint(context: PaintingContext, offset: Offset): void {
    const canvas = context.canvas;
    if (this.#needsClipping) {
      canvas.save();
      canvas.clipRect(offset.dx, offset.dy, this.size.width, this.size.height);
    }
    this.#painter.paint(canvas, offset);
    if (this.#needsClipping) canvas.restore();
  }

  /** 局部坐标 → 文本位置。 */
  getPositionForOffset(offset: Offset): TextPosition {
    return this.#painter.getPositionForOffset(offset);
  }
  /** 选区矩形(局部坐标)。 */
  getBoxesForSelection(start: number, end: number): Rect[] {
    return this.#painter.getBoxesForSelection(start, end);
  }
  /** 插入点的局部位置。 */
  getOffsetForCaret(offset: number): Offset {
    return this.#painter.getOffsetForCaret(offset);
  }

  override debugDescribe(): string {
    return `${super.debugDescribe()} "${this.#painter.text?.toPlainText().slice(0, 40) ?? ""}"`;
  }
}
