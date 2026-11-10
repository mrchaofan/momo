import { Alignment } from "../painting/alignment";
import { EdgeInsets } from "../painting/edge_insets";
import { Offset, Size } from "../painting/geometry";
import { RenderBox, RenderShiftedBox, type BoxParentData } from "./box";
import { BoxConstraints } from "./box_constraints";

/** 内边距。 */
export class RenderPadding extends RenderShiftedBox {
  #padding: EdgeInsets;
  constructor(padding: EdgeInsets, child: RenderBox | null = null) {
    super(child);
    this.#padding = padding;
  }
  get padding(): EdgeInsets {
    return this.#padding;
  }
  set padding(v: EdgeInsets) {
    if (v.equals(this.#padding)) return;
    this.#padding = v;
    this.markNeedsLayout();
  }
  protected override computeMinIntrinsicWidth(height: number): number {
    const p = this.#padding;
    return (this.child?.getMinIntrinsicWidth(Math.max(0, height - p.vertical)) ?? 0) + p.horizontal;
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    const p = this.#padding;
    return (this.child?.getMaxIntrinsicWidth(Math.max(0, height - p.vertical)) ?? 0) + p.horizontal;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    const p = this.#padding;
    return (this.child?.getMinIntrinsicHeight(Math.max(0, width - p.horizontal)) ?? 0) + p.vertical;
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    const p = this.#padding;
    return (this.child?.getMaxIntrinsicHeight(Math.max(0, width - p.horizontal)) ?? 0) + p.vertical;
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    const p = this.#padding;
    const child = this.child;
    if (child === null) return constraints.constrain(new Size(p.horizontal, p.vertical));
    const s = child.getDryLayout(constraints.deflate(p));
    return constraints.constrain(new Size(p.horizontal + s.width, p.vertical + s.height));
  }
  override performLayout(): void {
    const c = this.constraints;
    const p = this.#padding;
    const child = this.child;
    if (child === null) {
      this.size = c.constrain(new Size(p.horizontal, p.vertical));
      return;
    }
    child.layout(c.deflate(p), true);
    (child.parentData as BoxParentData).offset = new Offset(p.left, p.top);
    this.size = c.constrain(new Size(p.horizontal + child.size.width, p.vertical + child.size.height));
  }
}

/** 对齐子节点(Align/Center)。widthFactor/heightFactor 指定按子尺寸倍数收缩。 */
export class RenderPositionedBox extends RenderShiftedBox {
  #alignment: Alignment;
  #widthFactor: number | null;
  #heightFactor: number | null;
  constructor(o: { alignment?: Alignment; widthFactor?: number | null; heightFactor?: number | null } = {}, child: RenderBox | null = null) {
    super(child);
    this.#alignment = o.alignment ?? Alignment.center;
    this.#widthFactor = o.widthFactor ?? null;
    this.#heightFactor = o.heightFactor ?? null;
  }
  get alignment(): Alignment {
    return this.#alignment;
  }
  set alignment(v: Alignment) {
    if (v.equals(this.#alignment)) return;
    this.#alignment = v;
    this.markNeedsLayout();
  }
  set widthFactor(v: number | null) {
    if (v !== this.#widthFactor) {
      this.#widthFactor = v;
      this.markNeedsLayout();
    }
  }
  set heightFactor(v: number | null) {
    if (v !== this.#heightFactor) {
      this.#heightFactor = v;
      this.markNeedsLayout();
    }
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    const shrinkW = this.#widthFactor !== null || constraints.maxWidth === Infinity;
    const shrinkH = this.#heightFactor !== null || constraints.maxHeight === Infinity;
    const child = this.child;
    if (child !== null) {
      const s = child.getDryLayout(constraints.loosen());
      return constraints.constrain(new Size(shrinkW ? s.width * (this.#widthFactor ?? 1) : Infinity,
        shrinkH ? s.height * (this.#heightFactor ?? 1) : Infinity));
    }
    return constraints.constrain(new Size(shrinkW ? 0 : Infinity, shrinkH ? 0 : Infinity));
  }
  override performLayout(): void {
    const c = this.constraints;
    const shrinkW = this.#widthFactor !== null || c.maxWidth === Infinity;
    const shrinkH = this.#heightFactor !== null || c.maxHeight === Infinity;
    const child = this.child;
    if (child !== null) {
      child.layout(c.loosen(), true);
      this.size = c.constrain(new Size(shrinkW ? child.size.width * (this.#widthFactor ?? 1) : Infinity,
        shrinkH ? child.size.height * (this.#heightFactor ?? 1) : Infinity));
      const free = new Offset(this.size.width - child.size.width, this.size.height - child.size.height);
      (child.parentData as BoxParentData).offset = this.#alignment.alongOffset(free);
    } else {
      this.size = c.constrain(new Size(shrinkW ? 0 : Infinity, shrinkH ? 0 : Infinity));
    }
  }
}

/** 按父尺寸比例约束子节点。 */
export class RenderFractionallySizedBox extends RenderShiftedBox {
  #alignment: Alignment;
  #widthFactor: number | null;
  #heightFactor: number | null;
  constructor(o: { alignment?: Alignment; widthFactor?: number | null; heightFactor?: number | null } = {}, child: RenderBox | null = null) {
    super(child);
    this.#alignment = o.alignment ?? Alignment.center;
    this.#widthFactor = o.widthFactor ?? null;
    this.#heightFactor = o.heightFactor ?? null;
  }
  set alignment(v: Alignment) {
    this.#alignment = v;
    this.markNeedsLayout();
  }
  set widthFactor(v: number | null) {
    this.#widthFactor = v;
    this.markNeedsLayout();
  }
  set heightFactor(v: number | null) {
    this.#heightFactor = v;
    this.markNeedsLayout();
  }
  #inner(c: BoxConstraints): BoxConstraints {
    let minW = c.minWidth, maxW = c.maxWidth, minH = c.minHeight, maxH = c.maxHeight;
    if (this.#widthFactor !== null) minW = maxW = c.maxWidth * this.#widthFactor;
    if (this.#heightFactor !== null) minH = maxH = c.maxHeight * this.#heightFactor;
    return new BoxConstraints(minW, maxW, minH, maxH);
  }
  override performLayout(): void {
    const child = this.child;
    if (child !== null) {
      child.layout(this.#inner(this.constraints), true);
      this.size = this.constraints.constrain(child.size);
      (child.parentData as BoxParentData).offset = this.#alignment.alongOffset(
        new Offset(this.size.width - child.size.width, this.size.height - child.size.height));
    } else {
      this.size = this.constraints.constrain(this.#inner(this.constraints).constrain(Size.zero));
    }
  }
}
