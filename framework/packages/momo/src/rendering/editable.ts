import { Paint } from "momo:ui";
import type { ValueListenable } from "../foundation/change_notifier";
import { Color } from "../painting/color";
import { Offset, Rect, Size, type TextDirection } from "../painting/geometry";
import { TextPainter, type TextPosition } from "../painting/text_painter";
import { TextSpan } from "../painting/text_span";
import type { TextSelection } from "../services/text_editing";
import { RenderBox, type TextBaseline } from "./box";
import { BoxConstraints } from "./box_constraints";
import type { PaintingContext, PipelineOwner } from "./object";

const paint = new Paint();

/**
 * 可编辑文本的渲染:文本、选区背景、光标;单行时水平滚动、多行时垂直滚动以保持光标可见。
 * 编辑逻辑(输入法/键盘/手势)在 EditableText widget 中。
 */
export class RenderEditable extends RenderBox {
  readonly #painter: TextPainter;
  #selection: TextSelection | null;
  #showCursor: ValueListenable<boolean>;
  #hasFocus: boolean;
  #cursorColor: Color;
  #selectionColor: Color;
  #cursorWidth: number;
  #maxLines: number | null;
  #minLines: number | null;
  #scrollOffset = 0;
  #maxScroll = 0;
  #listener = (): void => this.markNeedsPaint();

  constructor(o: {
    text: TextSpan;
    selection?: TextSelection | null;
    showCursor: ValueListenable<boolean>;
    hasFocus?: boolean;
    cursorColor?: Color;
    selectionColor?: Color;
    cursorWidth?: number;
    maxLines?: number | null;
    minLines?: number | null;
    textAlign?: "left" | "right" | "center" | "start" | "end";
    textDirection?: TextDirection;
    textScaleFactor?: number;
  }) {
    super();
    this.#painter = new TextPainter({
      text: o.text,
      textAlign: o.textAlign ?? "start",
      textDirection: o.textDirection ?? "ltr",
      textScaleFactor: o.textScaleFactor ?? 1,
    });
    this.#selection = o.selection ?? null;
    this.#showCursor = o.showCursor;
    this.#hasFocus = o.hasFocus ?? false;
    this.#cursorColor = o.cursorColor ?? new Color(0xff1a73e8);
    this.#selectionColor = o.selectionColor ?? new Color(0x661a73e8);
    this.#cursorWidth = o.cursorWidth ?? 2;
    this.#maxLines = o.maxLines === undefined ? 1 : o.maxLines;
    this.#minLines = o.minLines ?? null;
  }

  get textPainter(): TextPainter {
    return this.#painter;
  }
  get text(): TextSpan {
    return this.#painter.text!;
  }
  set text(v: TextSpan) {
    const old = this.#painter.text;
    this.#painter.text = v;
    if (old === null || old.cacheKey !== v.cacheKey) this.markNeedsLayout();
  }
  get selection(): TextSelection | null {
    return this.#selection;
  }
  set selection(v: TextSelection | null) {
    if (v === this.#selection || (v !== null && this.#selection !== null && v.equals(this.#selection))) return;
    this.#selection = v;
    // 选区变化可能需要滚动以显示光标
    this.markNeedsLayout();
  }
  set showCursor(v: ValueListenable<boolean>) {
    if (v === this.#showCursor) return;
    if (this.attached) {
      this.#showCursor.removeListener(this.#listener);
      v.addListener(this.#listener);
    }
    this.#showCursor = v;
    this.markNeedsPaint();
  }
  get hasFocus(): boolean {
    return this.#hasFocus;
  }
  set hasFocus(v: boolean) {
    if (v !== this.#hasFocus) {
      this.#hasFocus = v;
      this.markNeedsPaint();
    }
  }
  set cursorColor(v: Color) {
    if (!v.equals(this.#cursorColor)) {
      this.#cursorColor = v;
      this.markNeedsPaint();
    }
  }
  set selectionColor(v: Color) {
    if (!v.equals(this.#selectionColor)) {
      this.#selectionColor = v;
      this.markNeedsPaint();
    }
  }
  set cursorWidth(v: number) {
    if (v !== this.#cursorWidth) {
      this.#cursorWidth = v;
      this.markNeedsLayout();
    }
  }
  get maxLines(): number | null {
    return this.#maxLines;
  }
  set maxLines(v: number | null) {
    if (v !== this.#maxLines) {
      this.#maxLines = v;
      this.markNeedsLayout();
    }
  }
  set minLines(v: number | null) {
    if (v !== this.#minLines) {
      this.#minLines = v;
      this.markNeedsLayout();
    }
  }
  set textAlign(v: "left" | "right" | "center" | "start" | "end") {
    if (v !== this.#painter.textAlign) {
      this.#painter.textAlign = v;
      this.markNeedsLayout();
    }
  }
  set textScaleFactor(v: number) {
    if (v !== this.#painter.textScaleFactor) {
      this.#painter.textScaleFactor = v;
      this.markNeedsLayout();
    }
  }

  get #singleLine(): boolean {
    return this.#maxLines === 1;
  }

  /** 当前滚动量(单行为水平, 多行为垂直)。 */
  get scrollOffset(): number {
    return this.#scrollOffset;
  }
  get maxScrollExtent(): number {
    return this.#maxScroll;
  }

  /** 单行高度。 */
  get preferredLineHeight(): number {
    return this.#painter.preferredLineHeight;
  }

  override attach(owner: PipelineOwner): void {
    super.attach(owner);
    this.#showCursor.addListener(this.#listener);
  }
  override detach(): void {
    this.#showCursor.removeListener(this.#listener);
    super.detach();
  }

  #layoutText(maxWidth: number): void {
    if (this.#singleLine) this.#painter.layout(0, Infinity);
    else this.#painter.layout(0, Math.max(0, maxWidth - this.#cursorWidth));
  }

  #preferredHeight(width: number): number {
    const lh = this.preferredLineHeight;
    if (this.#singleLine) return lh;
    this.#layoutText(width);
    let h = Math.max(this.#painter.height, lh);
    if (this.#minLines !== null) h = Math.max(h, this.#minLines * lh);
    if (this.#maxLines !== null) h = Math.min(h, this.#maxLines * lh);
    return h;
  }

  protected override computeMinIntrinsicWidth(_height: number): number {
    this.#painter.layout();
    return this.#painter.minIntrinsicWidth + this.#cursorWidth;
  }
  protected override computeMaxIntrinsicWidth(_height: number): number {
    this.#painter.layout();
    return this.#painter.maxIntrinsicWidth + this.#cursorWidth;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    return this.#preferredHeight(width);
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    return this.#preferredHeight(width);
  }
  protected override computeDistanceToActualBaseline(baseline: TextBaseline): number | null {
    this.#layoutText(this.constraints.maxWidth);
    return this.#painter.computeDistanceToActualBaseline(baseline) - (this.#singleLine ? 0 : this.#scrollOffset);
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    this.#layoutText(constraints.maxWidth);
    const width = constraints.hasBoundedWidth ? constraints.maxWidth : this.#painter.width + this.#cursorWidth;
    return constraints.constrain(new Size(width, this.#preferredHeight(constraints.maxWidth)));
  }

  override performLayout(): void {
    const c = this.constraints;
    this.#layoutText(c.maxWidth);
    const width = c.hasBoundedWidth ? c.maxWidth : this.#painter.width + this.#cursorWidth;
    this.size = c.constrain(new Size(width, this.#preferredHeight(c.maxWidth)));
    const contentMain = this.#singleLine ? this.#painter.width + this.#cursorWidth : this.#painter.height;
    const viewportMain = this.#singleLine ? this.size.width : this.size.height;
    this.#maxScroll = Math.max(0, contentMain - viewportMain);
    this.#ensureCaretVisible();
  }

  #ensureCaretVisible(): void {
    const sel = this.#selection;
    let s = Math.min(Math.max(this.#scrollOffset, 0), this.#maxScroll);
    if (sel !== null && sel.isValid) {
      const caret = this.#painter.getOffsetForCaret(sel.extentOffset, sel.affinity);
      if (this.#singleLine) {
        const x = caret.dx;
        if (x - s < 0) s = x;
        else if (x + this.#cursorWidth - s > this.size.width) s = x + this.#cursorWidth - this.size.width;
      } else {
        const top = caret.dy;
        const bottom = top + this.preferredLineHeight;
        if (top - s < 0) s = top;
        else if (bottom - s > this.size.height) s = bottom - this.size.height;
      }
    }
    this.#scrollOffset = Math.min(Math.max(s, 0), this.#maxScroll);
  }

  /** 内容坐标 → 局部坐标的偏移。 */
  get #paintOffset(): Offset {
    return this.#singleLine ? new Offset(-this.#scrollOffset, 0) : new Offset(0, -this.#scrollOffset);
  }

  /** 局部坐标的点 → 文本位置。 */
  getPositionForPoint(local: Offset): TextPosition {
    const po = this.#paintOffset;
    return this.#painter.getPositionForOffset(new Offset(local.dx - po.dx, local.dy - po.dy));
  }

  /** 插入点的光标矩形(局部坐标)。 */
  getLocalRectForCaret(offset: number, affinity: "downstream" | "upstream" = "downstream"): Rect {
    const caret = this.#painter.getOffsetForCaret(offset, affinity);
    const po = this.#paintOffset;
    return Rect.fromLTWH(caret.dx + po.dx, caret.dy + po.dy, this.#cursorWidth, this.preferredLineHeight);
  }

  /** 选区矩形(局部坐标)。 */
  getBoxesForSelection(selection: TextSelection): Rect[] {
    const po = this.#paintOffset;
    return this.#painter.getBoxesForSelection(selection.start, selection.end).map((r) => r.shift(po));
  }

  /** 单词边界。 */
  getWordBoundary(offset: number): { start: number; end: number } {
    return this.#painter.getWordBoundary(offset);
  }

  /** 行度量(用于上下移动/行首行尾)。 */
  computeLineMetrics(): ReturnType<TextPainter["computeLineMetrics"]> {
    return this.#painter.computeLineMetrics();
  }

  override hitTestSelf(): boolean {
    return true;
  }

  override paint(context: PaintingContext, offset: Offset): void {
    const canvas = context.canvas;
    const clip = this.#maxScroll > 0;
    if (clip) {
      canvas.save();
      canvas.clipRect(offset.dx, offset.dy, this.size.width, this.size.height);
    }
    const po = this.#paintOffset;
    const origin = new Offset(offset.dx + po.dx, offset.dy + po.dy);
    const sel = this.#selection;
    if (sel !== null && sel.isValid && !sel.isCollapsed && this.#hasFocus) {
      paint.color = this.#selectionColor.value;
      paint.style = "fill";
      paint.shader = null;
      paint.maskBlurSigma = 0;
      for (const r of this.#painter.getBoxesForSelection(sel.start, sel.end)) {
        canvas.drawRect(origin.dx + r.left, origin.dy + r.top, r.width, r.height, paint);
      }
    }
    this.#painter.paint(canvas, origin);
    if (sel !== null && sel.isValid && sel.isCollapsed && this.#hasFocus && this.#showCursor.value) {
      const caret = this.#painter.getOffsetForCaret(sel.extentOffset, sel.affinity);
      paint.color = this.#cursorColor.value;
      paint.style = "fill";
      paint.shader = null;
      paint.maskBlurSigma = 0;
      canvas.drawRect(origin.dx + caret.dx, origin.dy + caret.dy, this.#cursorWidth, this.preferredLineHeight, paint);
    }
    if (clip) canvas.restore();
  }
}
