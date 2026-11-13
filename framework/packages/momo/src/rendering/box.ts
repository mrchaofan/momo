import { HitTestEntry, HitTestResult } from "../gestures/hit_test";
import { Offset, Rect, Size } from "../painting/geometry";
import { Matrix4 } from "../painting/matrix4";
import { BoxConstraints } from "./box_constraints";
import { PaintingContext, ParentData, RenderObject } from "./object";

/** 盒模型子节点的父数据:相对父节点的偏移。 */
export class BoxParentData extends ParentData {
  offset: Offset = Offset.zero;
}

/** 多子节点容器的父数据。 */
export class ContainerBoxParentData extends BoxParentData {}

/** 盒模型命中项(带局部坐标)。 */
export class BoxHitTestEntry extends HitTestEntry {
  constructor(target: RenderBox, readonly localPosition: Offset) {
    super(target);
  }
}

/** 命中行为。 */
export type HitTestBehavior = "deferToChild" | "opaque" | "translucent";

/** 基线类型。 */
export type TextBaseline = "alphabetic" | "ideographic";

const enum IntrinsicKind {
  minWidth = 0,
  maxWidth = 1,
  minHeight = 2,
  maxHeight = 3,
}

/**
 * 盒模型渲染对象:BoxConstraints → Size;父通过 parentData.offset 摆放。
 */
export abstract class RenderBox extends RenderObject {
  #size: Size | null = null;
  #intrinsicCache: Map<number, number> | null = null;
  #dryLayoutCache: Map<string, Size> | null = null;
  #baselineCache: Map<TextBaseline, number | null> | null = null;

  override setupParentData(child: RenderObject): void {
    if (!(child.parentData instanceof BoxParentData)) child.parentData = new BoxParentData();
  }

  /** 布局后的尺寸。 */
  get size(): Size {
    if (this.#size === null) throw new Error(`${this.toStringShort()} has not been laid out (no size)`);
    return this.#size;
  }
  set size(v: Size) {
    this.#size = v;
  }
  get hasSize(): boolean {
    return this.#size !== null;
  }

  override get constraints(): BoxConstraints {
    return super.constraints as BoxConstraints;
  }

  override get paintBounds(): Rect {
    return Rect.fromLTWH(0, 0, this.size.width, this.size.height);
  }

  override markNeedsLayout(): void {
    const hadCache = (this.#intrinsicCache !== null && this.#intrinsicCache.size > 0) ||
      (this.#dryLayoutCache !== null && this.#dryLayoutCache.size > 0) ||
      (this.#baselineCache !== null && this.#baselineCache.size > 0);
    this.#intrinsicCache?.clear();
    this.#dryLayoutCache?.clear();
    this.#baselineCache?.clear();
    if (hadCache && this.parent !== null) {
      // 父节点可能依赖本节点的固有尺寸/基线, 一并重新布局。
      this.markParentNeedsLayout();
      return;
    }
    super.markNeedsLayout();
  }

  #intrinsic(kind: IntrinsicKind, extent: number, compute: (extent: number) => number): number {
    const key = kind * 1e9 + (extent === Infinity ? 1e8 : extent);
    if (this.#intrinsicCache === null) this.#intrinsicCache = new Map();
    let v = this.#intrinsicCache.get(key);
    if (v === undefined) {
      v = compute.call(this, extent);
      this.#intrinsicCache.set(key, v);
    }
    return v;
  }

  /** 在给定高度下的最小固有宽度。 */
  getMinIntrinsicWidth(height: number): number {
    return this.#intrinsic(IntrinsicKind.minWidth, height, this.computeMinIntrinsicWidth);
  }
  getMaxIntrinsicWidth(height: number): number {
    return this.#intrinsic(IntrinsicKind.maxWidth, height, this.computeMaxIntrinsicWidth);
  }
  getMinIntrinsicHeight(width: number): number {
    return this.#intrinsic(IntrinsicKind.minHeight, width, this.computeMinIntrinsicHeight);
  }
  getMaxIntrinsicHeight(width: number): number {
    return this.#intrinsic(IntrinsicKind.maxHeight, width, this.computeMaxIntrinsicHeight);
  }
  protected computeMinIntrinsicWidth(_height: number): number {
    return 0;
  }
  protected computeMaxIntrinsicWidth(_height: number): number {
    return 0;
  }
  protected computeMinIntrinsicHeight(_width: number): number {
    return 0;
  }
  protected computeMaxIntrinsicHeight(_width: number): number {
    return 0;
  }

  /** 不实际布局地计算在 constraints 下的尺寸。 */
  getDryLayout(constraints: BoxConstraints): Size {
    const key = `${constraints.minWidth},${constraints.maxWidth},${constraints.minHeight},${constraints.maxHeight}`;
    if (this.#dryLayoutCache === null) this.#dryLayoutCache = new Map();
    let s = this.#dryLayoutCache.get(key);
    if (s === undefined) {
      s = this.computeDryLayout(constraints);
      this.#dryLayoutCache.set(key, s);
    }
    return s;
  }
  protected computeDryLayout(constraints: BoxConstraints): Size {
    return constraints.smallest;
  }

  override performResize(): void {
    this.size = this.computeDryLayout(this.constraints);
  }

  override performLayout(): void {
    if (!this.sizedByParent) this.size = this.constraints.smallest;
  }

  /** 到首个基线的距离(无则 null)。须在布局后调用。 */
  getDistanceToBaseline(baseline: TextBaseline, onlyReal = false): number | null {
    const r = this.getDistanceToActualBaseline(baseline);
    if (r === null && !onlyReal) return this.size.height;
    return r;
  }
  getDistanceToActualBaseline(baseline: TextBaseline): number | null {
    if (this.#baselineCache === null) this.#baselineCache = new Map();
    if (!this.#baselineCache.has(baseline)) this.#baselineCache.set(baseline, this.computeDistanceToActualBaseline(baseline));
    return this.#baselineCache.get(baseline) ?? null;
  }
  protected computeDistanceToActualBaseline(_baseline: TextBaseline): number | null {
    return null;
  }

  /**
   * 命中测试:position 在本节点局部坐标。命中时把自身加入 result 并返回 true。
   */
  hitTest(result: HitTestResult, position: Offset): boolean {
    if (this.size.contains(position)) {
      if (this.hitTestChildren(result, position) || this.hitTestSelf(position)) {
        result.add(new BoxHitTestEntry(this, position));
        return true;
      }
    }
    return false;
  }
  hitTestSelf(_position: Offset): boolean {
    return false;
  }
  /** 自身可命中(以中心点代表整个盒)时, 整个盒记为可命中区域。 */
  override paintHitRegion(context: PaintingContext, offset: Offset): void {
    const { width, height } = this.size;
    if (width > 0 && height > 0 && this.hitTestSelf(new Offset(width / 2, height / 2))) {
      context.canvas.addHitRegion(offset.dx, offset.dy, width, height);
    }
  }
  hitTestChildren(_result: HitTestResult, _position: Offset): boolean {
    return false;
  }

  override applyPaintTransform(child: RenderObject, transform: Matrix4): void {
    const pd = child.parentData;
    if (pd instanceof BoxParentData) transform.translate(pd.offset.dx, pd.offset.dy);
  }

  /** 全局(或 ancestor)坐标 → 局部坐标。 */
  globalToLocal(point: Offset, ancestor: RenderObject | null = null): Offset {
    const inv = this.getTransformTo(ancestor).inverted();
    return inv === null ? Offset.zero : inv.transformPoint(point);
  }
  /** 局部坐标 → 全局(或 ancestor)坐标。 */
  localToGlobal(point: Offset, ancestor: RenderObject | null = null): Offset {
    return this.getTransformTo(ancestor).transformPoint(point);
  }

  override debugDescribe(): string {
    return this.hasSize ? ` size=${this.size}` : " (no size)";
  }
}

/** 单子节点盒模型基类:管理 child 的收养/释放与遍历。 */
export abstract class RenderBoxWithChild extends RenderBox {
  #child: RenderBox | null = null;

  constructor(child: RenderBox | null = null) {
    super();
    this.child = child;
  }

  get child(): RenderBox | null {
    return this.#child;
  }
  set child(v: RenderBox | null) {
    if (this.#child === v) return;
    if (this.#child !== null) this.dropChild(this.#child);
    this.#child = v;
    if (v !== null) this.adoptChild(v);
  }

  override visitChildren(visitor: (child: RenderObject) => void): void {
    if (this.#child !== null) visitor(this.#child);
  }
}

/** 尺寸等于子节点的代理盒(无子时取最小尺寸)。 */
export class RenderProxyBox extends RenderBoxWithChild {
  protected override computeMinIntrinsicWidth(height: number): number {
    return this.child?.getMinIntrinsicWidth(height) ?? 0;
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    return this.child?.getMaxIntrinsicWidth(height) ?? 0;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    return this.child?.getMinIntrinsicHeight(width) ?? 0;
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    return this.child?.getMaxIntrinsicHeight(width) ?? 0;
  }
  protected override computeDistanceToActualBaseline(baseline: TextBaseline): number | null {
    return this.child?.getDistanceToActualBaseline(baseline) ?? null;
  }
  protected override computeDryLayout(constraints: BoxConstraints): Size {
    return this.child?.getDryLayout(constraints) ?? this.computeSizeForNoChild(constraints);
  }
  override performLayout(): void {
    const child = this.child;
    if (child !== null) {
      child.layout(this.constraints, true);
      this.size = child.size;
    } else {
      this.size = this.computeSizeForNoChild(this.constraints);
    }
  }
  protected computeSizeForNoChild(constraints: BoxConstraints): Size {
    return constraints.smallest;
  }
  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    return this.child?.hitTest(result, position) ?? false;
  }
  override paint(context: PaintingContext, offset: Offset): void {
    if (this.child !== null) context.paintChild(this.child, offset);
  }
}

/** 带命中行为的代理盒。 */
export class RenderProxyBoxWithHitTestBehavior extends RenderProxyBox {
  behavior: HitTestBehavior;
  constructor(behavior: HitTestBehavior = "deferToChild", child: RenderBox | null = null) {
    super(child);
    this.behavior = behavior;
  }
  override hitTest(result: HitTestResult, position: Offset): boolean {
    let hit = false;
    if (this.size.contains(position)) {
      hit = this.hitTestChildren(result, position) || this.hitTestSelf(position);
      if (hit || this.behavior === "translucent") result.add(new BoxHitTestEntry(this, position));
    }
    return hit;
  }
  override hitTestSelf(_position: Offset): boolean {
    return this.behavior === "opaque";
  }
}

/** 子节点有偏移的单子盒(如 Padding、Align)。 */
export abstract class RenderShiftedBox extends RenderBoxWithChild {
  protected override computeMinIntrinsicWidth(height: number): number {
    return this.child?.getMinIntrinsicWidth(height) ?? 0;
  }
  protected override computeMaxIntrinsicWidth(height: number): number {
    return this.child?.getMaxIntrinsicWidth(height) ?? 0;
  }
  protected override computeMinIntrinsicHeight(width: number): number {
    return this.child?.getMinIntrinsicHeight(width) ?? 0;
  }
  protected override computeMaxIntrinsicHeight(width: number): number {
    return this.child?.getMaxIntrinsicHeight(width) ?? 0;
  }
  protected override computeDistanceToActualBaseline(baseline: TextBaseline): number | null {
    const child = this.child;
    if (child === null) return null;
    const r = child.getDistanceToActualBaseline(baseline);
    return r === null ? null : r + (child.parentData as BoxParentData).offset.dy;
  }
  override paint(context: PaintingContext, offset: Offset): void {
    const child = this.child;
    if (child !== null) context.paintChild(child, offset.plus((child.parentData as BoxParentData).offset));
  }
  override hitTestChildren(result: HitTestResult, position: Offset): boolean {
    const child = this.child;
    if (child === null) return false;
    const offset = (child.parentData as BoxParentData).offset;
    return result.addWithPaintOffset(offset, position, (r, p) => child.hitTest(r, p));
  }
}

/**
 * 多子节点盒模型容器(以数组保存子节点, 顺序即绘制顺序)。
 */
export abstract class ContainerRenderBox<PD extends ContainerBoxParentData = ContainerBoxParentData> extends RenderBox {
  readonly children: RenderBox[] = [];

  protected abstract createParentData(): PD;

  override setupParentData(child: RenderObject): void {
    if (!(child.parentData instanceof ContainerBoxParentData)) child.parentData = this.createParentData();
  }

  /** 子节点的父数据。 */
  parentDataOf(child: RenderBox): PD {
    return child.parentData as PD;
  }

  get childCount(): number {
    return this.children.length;
  }
  get firstChild(): RenderBox | null {
    return this.children[0] ?? null;
  }
  get lastChild(): RenderBox | null {
    return this.children[this.children.length - 1] ?? null;
  }
  childAfter(child: RenderBox): RenderBox | null {
    const i = this.children.indexOf(child);
    return i >= 0 ? this.children[i + 1] ?? null : null;
  }

  /** 插入到 after 之后(after 为 null 表示最前)。 */
  insert(child: RenderBox, after: RenderBox | null = null): void {
    this.adoptChild(child);
    const index = after === null ? 0 : this.children.indexOf(after) + 1;
    this.children.splice(index, 0, child);
  }
  /** 追加到末尾。 */
  add(child: RenderBox): void {
    this.insert(child, this.lastChild);
  }
  addAll(children: readonly RenderBox[]): void {
    for (const c of children) this.add(c);
  }
  remove(child: RenderBox): void {
    const i = this.children.indexOf(child);
    if (i < 0) return;
    this.children.splice(i, 1);
    this.dropChild(child);
  }
  removeAll(): void {
    for (const c of this.children.splice(0)) this.dropChild(c);
  }
  /** 移动到 after 之后。 */
  move(child: RenderBox, after: RenderBox | null = null): void {
    const i = this.children.indexOf(child);
    if (i < 0) return;
    this.children.splice(i, 1);
    const index = after === null ? 0 : this.children.indexOf(after) + 1;
    this.children.splice(index, 0, child);
    this.markNeedsLayout();
  }

  override visitChildren(visitor: (child: RenderObject) => void): void {
    const c = this.children;
    for (let i = 0; i < c.length; i++) visitor(c[i]!);
  }

  /** 逆序(从上到下)命中测试子节点。 */
  defaultHitTestChildren(result: HitTestResult, position: Offset): boolean {
    for (let i = this.children.length - 1; i >= 0; i--) {
      const child = this.children[i]!;
      const offset = (child.parentData as BoxParentData).offset;
      if (result.addWithPaintOffset(offset, position, (r, p) => child.hitTest(r, p))) return true;
    }
    return false;
  }

  /** 顺序绘制子节点。 */
  defaultPaint(context: PaintingContext, offset: Offset): void {
    const c = this.children;
    for (let i = 0; i < c.length; i++) {
      const child = c[i]!;
      const o = (child.parentData as BoxParentData).offset;
      context.paintChild(child, new Offset(offset.dx + o.dx, offset.dy + o.dy));
    }
  }

  /** 首个有基线的子节点的基线。 */
  defaultComputeDistanceToFirstActualBaseline(baseline: TextBaseline): number | null {
    for (const child of this.children) {
      const r = child.getDistanceToActualBaseline(baseline);
      if (r !== null) return r + (child.parentData as BoxParentData).offset.dy;
    }
    return null;
  }

  /** 子节点中最高(最小)的基线。 */
  defaultComputeDistanceToHighestActualBaseline(baseline: TextBaseline): number | null {
    let result: number | null = null;
    for (const child of this.children) {
      let r = child.getDistanceToActualBaseline(baseline);
      if (r !== null) {
        r += (child.parentData as BoxParentData).offset.dy;
        result = result === null ? r : Math.min(result, r);
      }
    }
    return result;
  }
}

/** 渲染对象是否支持单子节点接口。 */
export function isSingleChildRenderObject(o: RenderObject): o is RenderObject & { child: RenderObject | null } {
  return o instanceof RenderBoxWithChild || "child" in o;
}
