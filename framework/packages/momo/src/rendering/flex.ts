import type { HitTestResult } from "../gestures/hit_test";
import { Offset, Rect, Size, type Axis, type Clip, type VerticalDirection } from "../painting/geometry";
import { ContainerBoxParentData, ContainerRenderBox, RenderBox, type TextBaseline } from "./box";
import { BoxConstraints } from "./box_constraints";
import type { PaintingContext } from "./object";

/** 弹性子节点如何填充分配到的空间。 */
export type FlexFit = "tight" | "loose";
/** 主轴尺寸:尽量大 / 包裹内容。 */
export type MainAxisSize = "min" | "max";
/** 主轴对齐。 */
export type MainAxisAlignment = "start" | "end" | "center" | "spaceBetween" | "spaceAround" | "spaceEvenly";
/** 交叉轴对齐。 */
export type CrossAxisAlignment = "start" | "end" | "center" | "stretch" | "baseline";

/** Flex 子节点父数据。 */
export class FlexParentData extends ContainerBoxParentData {
  /** 弹性系数;0 或 null 为非弹性。 */
  flex: number | null = null;
  fit: FlexFit = "tight";
}

/**
 * 一维弹性布局(Row/Column):先布局非弹性子节点, 剩余主轴空间按 flex 分配给弹性子节点。
 */
export class RenderFlex extends ContainerRenderBox<FlexParentData> {
  #direction: Axis;
  #mainAxisAlignment: MainAxisAlignment;
  #mainAxisSize: MainAxisSize;
  #crossAxisAlignment: CrossAxisAlignment;
  #verticalDirection: VerticalDirection;
  #textBaseline: TextBaseline | null;
  #clipBehavior: Clip;
  #overflow = 0;

  constructor(o: {
    direction?: Axis;
    mainAxisAlignment?: MainAxisAlignment;
    mainAxisSize?: MainAxisSize;
    crossAxisAlignment?: CrossAxisAlignment;
    verticalDirection?: VerticalDirection;
    textBaseline?: TextBaseline | null;
    clipBehavior?: Clip;
    children?: readonly RenderBox[];
  } = {}) {
    super();
    this.#direction = o.direction ?? "horizontal";
    this.#mainAxisAlignment = o.mainAxisAlignment ?? "start";
    this.#mainAxisSize = o.mainAxisSize ?? "max";
    this.#crossAxisAlignment = o.crossAxisAlignment ?? "center";
    this.#verticalDirection = o.verticalDirection ?? "down";
    this.#textBaseline = o.textBaseline ?? null;
    this.#clipBehavior = o.clipBehavior ?? "none";
    if (o.children) this.addAll(o.children);
  }

  protected createParentData(): FlexParentData {
    return new FlexParentData();
  }

  get direction(): Axis {
    return this.#direction;
  }
  set direction(v: Axis) {
    if (v !== this.#direction) {
      this.#direction = v;
      this.markNeedsLayout();
    }
  }
  set mainAxisAlignment(v: MainAxisAlignment) {
    if (v !== this.#mainAxisAlignment) {
      this.#mainAxisAlignment = v;
      this.markNeedsLayout();
    }
  }
  set mainAxisSize(v: MainAxisSize) {
    if (v !== this.#mainAxisSize) {
      this.#mainAxisSize = v;
      this.markNeedsLayout();
    }
  }
  set crossAxisAlignment(v: CrossAxisAlignment) {
    if (v !== this.#crossAxisAlignment) {
      this.#crossAxisAlignment = v;
      this.markNeedsLayout();
    }
  }
  set verticalDirection(v: VerticalDirection) {
    if (v !== this.#verticalDirection) {
      this.#verticalDirection = v;
      this.markNeedsLayout();
    }
  }
  set textBaseline(v: TextBaseline | null) {
    if (v !== this.#textBaseline) {
      this.#textBaseline = v;
      this.markNeedsLayout();
    }
  }
  set clipBehavior(v: Clip) {
    if (v !== this.#clipBehavior) {
      this.#clipBehavior = v;
      this.markNeedsPaint();
    }
  }

  /** 主轴溢出量(调试)。 */
  get overflow(): number {
    return this.#overflow;
  }

  #flexOf(child: RenderBox): number {
    return (child.parentData as FlexParentData).flex ?? 0;
  }
  #main(s: Size): number {
    return this.#direction === "horizontal" ? s.width : s.height;
  }
  #cross(s: Size): number {
    return this.#direction === "horizontal" ? s.height : s.width;
  }

  #intrinsic(extent: number, alongMain: boolean, minimal: boolean): number {
    const horizontal = this.#direction === "horizontal";
    const mainSize = (c: RenderBox, cross: number): number => horizontal
      ? (minimal ? c.getMinIntrinsicWidth(cross) : c.getMaxIntrinsicWidth(cross))
      : (minimal ? c.getMinIntrinsicHeight(cross) : c.getMaxIntrinsicHeight(cross));
    const crossSize = (c: RenderBox, main: number): number => horizontal
      ? (minimal ? c.getMinIntrinsicHeight(main) : c.getMaxIntrinsicHeight(main))
      : (minimal ? c.getMinIntrinsicWidth(main) : c.getMaxIntrinsicWidth(main));
    if (alongMain) {
      let totalFlex = 0;
      let inflexible = 0;
      let maxFlexFraction = 0;
      for (const c of this.children) {
        const flex = this.#flexOf(c);
        totalFlex += flex;
        const s = mainSize(c, extent);
        if (flex > 0) maxFlexFraction = Math.max(maxFlexFraction, s / flex);
        else inflexible += s;
      }
      return maxFlexFraction * totalFlex + inflexible;
    }
    let totalFlex = 0;
    let inflexibleSpace = 0;
    let maxCross = 0;
    for (const c of this.children) {
      const flex = this.#flexOf(c);
      totalFlex += flex;
      if (flex === 0) {
        const m = horizontal ? c.getMaxIntrinsicWidth(Infinity) : c.getMaxIntrinsicHeight(Infinity);
        inflexibleSpace += m;
        maxCross = Math.max(maxCross, crossSize(c, m));
      }
    }
    const spacePerFlex = totalFlex > 0 ? Math.max(0, (extent - inflexibleSpace) / totalFlex) : 0;
    for (const c of this.children) {
      const flex = this.#flexOf(c);
      if (flex > 0) maxCross = Math.max(maxCross, crossSize(c, Number.isFinite(spacePerFlex) ? spacePerFlex * flex : Infinity));
    }
    return maxCross;
  }

  protected override computeMinIntrinsicWidth(height: number): number {
    return this.#intrinsic(height, this.#direction === "horizontal", true);
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    return this.#intrinsic(height, this.#direction === "horizontal", false);
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    return this.#intrinsic(width, this.#direction === "vertical", true);
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    return this.#intrinsic(width, this.#direction === "vertical", false);
  }

  protected override computeDistanceToActualBaseline(baseline: TextBaseline): number | null {
    return this.#direction === "horizontal"
      ? this.defaultComputeDistanceToHighestActualBaseline(baseline)
      : this.defaultComputeDistanceToFirstActualBaseline(baseline);
  }

  override performLayout(): void {
    const c = this.constraints;
    const horizontal = this.#direction === "horizontal";
    const maxMain = horizontal ? c.maxWidth : c.maxHeight;
    const maxCross = horizontal ? c.maxHeight : c.maxWidth;
    const canFlex = maxMain < Infinity;
    const stretch = this.#crossAxisAlignment === "stretch";
    const baselineAlign = this.#crossAxisAlignment === "baseline" && horizontal;
    const baseline = this.#textBaseline ?? "alphabetic";

    const innerFor = (minMain: number, maxMainChild: number): BoxConstraints => {
      if (horizontal) {
        return stretch ? new BoxConstraints(minMain, maxMainChild, maxCross, maxCross) : new BoxConstraints(minMain, maxMainChild, 0, maxCross);
      }
      return stretch ? new BoxConstraints(maxCross, maxCross, minMain, maxMainChild) : new BoxConstraints(0, maxCross, minMain, maxMainChild);
    };

    let totalFlex = 0;
    let allocated = 0;
    let crossSize = 0;
    let maxAbove = 0;
    let maxBelow = 0;
    const children = this.children;
    const noteBaseline = (child: RenderBox): void => {
      if (!baselineAlign) return;
      const d = child.getDistanceToBaseline(baseline, true);
      if (d !== null) {
        maxAbove = Math.max(maxAbove, d);
        maxBelow = Math.max(maxBelow, child.size.height - d);
        crossSize = Math.max(crossSize, maxAbove + maxBelow);
      }
    };
    for (let i = 0; i < children.length; i++) {
      const child = children[i]!;
      const flex = this.#flexOf(child);
      if (flex > 0) {
        totalFlex += flex;
      } else {
        child.layout(innerFor(0, Infinity), true);
        allocated += this.#main(child.size);
        crossSize = Math.max(crossSize, this.#cross(child.size));
        noteBaseline(child);
      }
    }
    if (totalFlex > 0) {
      const freeSpace = Math.max(0, (canFlex ? maxMain : 0) - allocated);
      const spacePerFlex = canFlex ? freeSpace / totalFlex : NaN;
      for (let i = 0; i < children.length; i++) {
        const child = children[i]!;
        const flex = this.#flexOf(child);
        if (flex <= 0) continue;
        const maxExtent = canFlex ? spacePerFlex * flex : Infinity;
        const fit = (child.parentData as FlexParentData).fit;
        const minExtent = fit === "tight" && Number.isFinite(maxExtent) ? maxExtent : 0;
        child.layout(innerFor(minExtent, maxExtent), true);
        allocated += this.#main(child.size);
        crossSize = Math.max(crossSize, this.#cross(child.size));
        noteBaseline(child);
      }
    }

    const idealMain = canFlex && this.#mainAxisSize === "max" ? maxMain : allocated;
    const size = horizontal ? c.constrain(new Size(idealMain, crossSize)) : c.constrain(new Size(crossSize, idealMain));
    this.size = size;
    const actualMain = this.#main(size);
    const actualCross = this.#cross(size);
    const delta = actualMain - allocated;
    this.#overflow = Math.max(0, -delta);
    const remaining = Math.max(0, delta);

    const n = children.length;
    let leading = 0;
    let between = 0;
    switch (this.#mainAxisAlignment) {
      case "start":
        break;
      case "end":
        leading = remaining;
        break;
      case "center":
        leading = remaining / 2;
        break;
      case "spaceBetween":
        between = n > 1 ? remaining / (n - 1) : 0;
        break;
      case "spaceAround":
        between = n > 0 ? remaining / n : 0;
        leading = between / 2;
        break;
      case "spaceEvenly":
        between = n > 0 ? remaining / (n + 1) : 0;
        leading = between;
        break;
    }

    const flip = !horizontal && this.#verticalDirection === "up";
    let mainPos = flip ? actualMain - leading : leading;
    for (let i = 0; i < n; i++) {
      const child = children[i]!;
      const childMain = this.#main(child.size);
      const childCross = this.#cross(child.size);
      let crossPos = 0;
      switch (this.#crossAxisAlignment) {
        case "start":
        case "stretch":
          crossPos = 0;
          break;
        case "end":
          crossPos = actualCross - childCross;
          break;
        case "center":
          crossPos = (actualCross - childCross) / 2;
          break;
        case "baseline":
          if (horizontal) {
            const d = child.getDistanceToBaseline(baseline, true);
            crossPos = d !== null ? maxAbove - d : 0;
          }
          break;
      }
      if (flip) mainPos -= childMain;
      const pd = child.parentData as FlexParentData;
      pd.offset = horizontal ? new Offset(mainPos, crossPos) : new Offset(crossPos, mainPos);
      if (flip) mainPos -= between;
      else mainPos += childMain + between;
    }
  }

  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    return this.defaultHitTestChildren(result, position);
  }

  override paint(context: PaintingContext, offset: Offset): void {
    if (this.#overflow > 0 && this.#clipBehavior !== "none") {
      context.pushClipRect(this.needsCompositing, offset, Rect.fromLTWH(0, 0, this.size.width, this.size.height),
        (ctx, o) => this.defaultPaint(ctx, o), this.#clipBehavior);
      return;
    }
    this.defaultPaint(context, offset);
  }

  override debugDescribe(): string {
    return `${super.debugDescribe()} ${this.#direction}${this.#overflow > 0 ? ` OVERFLOW ${this.#overflow.toFixed(1)}` : ""}`;
  }
}
