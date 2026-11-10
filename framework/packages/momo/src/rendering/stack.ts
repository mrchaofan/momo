import type { HitTestResult } from "../gestures/hit_test";
import { Alignment } from "../painting/alignment";
import { Offset, Rect, Size, type Clip } from "../painting/geometry";
import { ContainerBoxParentData, ContainerRenderBox, RenderBox, type TextBaseline } from "./box";
import { BoxConstraints } from "./box_constraints";
import type { PaintingContext } from "./object";

/** 非定位子节点的约束方式。 */
export type StackFit = "loose" | "expand" | "passthrough";

/** Stack 子节点父数据(定位参数)。 */
export class StackParentData extends ContainerBoxParentData {
  top: number | null = null;
  right: number | null = null;
  bottom: number | null = null;
  left: number | null = null;
  width: number | null = null;
  height: number | null = null;
  get isPositioned(): boolean {
    return this.top !== null || this.right !== null || this.bottom !== null || this.left !== null || this.width !== null || this.height !== null;
  }
}

/** 层叠布局:非定位子节点决定尺寸, 定位子节点按 top/left/right/bottom 摆放。 */
export class RenderStack extends ContainerRenderBox<StackParentData> {
  #alignment: Alignment;
  #fit: StackFit;
  #clipBehavior: Clip;
  #hasVisualOverflow = false;

  constructor(o: { alignment?: Alignment; fit?: StackFit; clipBehavior?: Clip; children?: readonly RenderBox[] } = {}) {
    super();
    this.#alignment = o.alignment ?? Alignment.topLeft;
    this.#fit = o.fit ?? "loose";
    this.#clipBehavior = o.clipBehavior ?? "hardEdge";
    if (o.children) this.addAll(o.children);
  }

  protected createParentData(): StackParentData {
    return new StackParentData();
  }

  set alignment(v: Alignment) {
    if (!v.equals(this.#alignment)) {
      this.#alignment = v;
      this.markNeedsLayout();
    }
  }
  set fit(v: StackFit) {
    if (v !== this.#fit) {
      this.#fit = v;
      this.markNeedsLayout();
    }
  }
  set clipBehavior(v: Clip) {
    if (v !== this.#clipBehavior) {
      this.#clipBehavior = v;
      this.markNeedsPaint();
    }
  }

  protected override computeMinIntrinsicWidth(height: number): number {
    let w = 0;
    for (const c of this.children) if (!(c.parentData as StackParentData).isPositioned) w = Math.max(w, c.getMinIntrinsicWidth(height));
    return w;
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    let w = 0;
    for (const c of this.children) if (!(c.parentData as StackParentData).isPositioned) w = Math.max(w, c.getMaxIntrinsicWidth(height));
    return w;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    let h = 0;
    for (const c of this.children) if (!(c.parentData as StackParentData).isPositioned) h = Math.max(h, c.getMinIntrinsicHeight(width));
    return h;
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    let h = 0;
    for (const c of this.children) if (!(c.parentData as StackParentData).isPositioned) h = Math.max(h, c.getMaxIntrinsicHeight(width));
    return h;
  }
  protected override computeDistanceToActualBaseline(baseline: TextBaseline): number | null {
    return this.defaultComputeDistanceToHighestActualBaseline(baseline);
  }

  override performLayout(): void {
    const c = this.constraints;
    this.#hasVisualOverflow = false;
    let hasNonPositioned = false;
    let width = c.minWidth;
    let height = c.minHeight;
    const npc = this.#fit === "loose" ? c.loosen() : this.#fit === "expand" ? BoxConstraints.tight(c.biggest) : c;
    for (const child of this.children) {
      const pd = child.parentData as StackParentData;
      if (pd.isPositioned) continue;
      hasNonPositioned = true;
      child.layout(npc, true);
      width = Math.max(width, child.size.width);
      height = Math.max(height, child.size.height);
    }
    const size = hasNonPositioned ? new Size(width, height) : c.biggest;
    this.size = size;
    for (const child of this.children) {
      const pd = child.parentData as StackParentData;
      if (!pd.isPositioned) {
        pd.offset = this.#alignment.alongOffset(new Offset(size.width - child.size.width, size.height - child.size.height));
        continue;
      }
      let cc = new BoxConstraints();
      if (pd.left !== null && pd.right !== null) cc = cc.tighten({ width: size.width - pd.right - pd.left });
      else if (pd.width !== null) cc = cc.tighten({ width: pd.width });
      if (pd.top !== null && pd.bottom !== null) cc = cc.tighten({ height: size.height - pd.bottom - pd.top });
      else if (pd.height !== null) cc = cc.tighten({ height: pd.height });
      child.layout(cc, true);
      const aligned = this.#alignment.alongOffset(new Offset(size.width - child.size.width, size.height - child.size.height));
      const x = pd.left !== null ? pd.left : pd.right !== null ? size.width - pd.right - child.size.width : aligned.dx;
      const y = pd.top !== null ? pd.top : pd.bottom !== null ? size.height - pd.bottom - child.size.height : aligned.dy;
      if (x < 0 || x + child.size.width > size.width || y < 0 || y + child.size.height > size.height) this.#hasVisualOverflow = true;
      pd.offset = new Offset(x, y);
    }
  }

  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    return this.defaultHitTestChildren(result, position);
  }

  override paint(context: PaintingContext, offset: Offset): void {
    if (this.#hasVisualOverflow && this.#clipBehavior !== "none") {
      context.pushClipRect(this.needsCompositing, offset, Rect.fromLTWH(0, 0, this.size.width, this.size.height),
        (ctx, o) => this.defaultPaint(ctx, o), this.#clipBehavior);
      return;
    }
    this.defaultPaint(context, offset);
  }
}
