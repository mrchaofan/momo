import type { Key } from "../foundation/key";
import type {
  PointerCancelEvent, PointerDownEvent, PointerEnterEvent, PointerExitEvent, PointerHoverEvent, PointerMoveEvent,
  PointerSignalEvent, PointerUpEvent,
} from "../gestures/events";
import { Alignment } from "../painting/alignment";
import { BorderRadius } from "../painting/borders";
import type { Color } from "../painting/color";
import { BoxDecoration, type Decoration } from "../painting/decoration";
import { EdgeInsets } from "../painting/edge_insets";
import { Offset, Size, type Axis, type Clip, type VerticalDirection } from "../painting/geometry";
import { Matrix4 } from "../painting/matrix4";
import type { HitTestBehavior, TextBaseline } from "../rendering/box";
import { BoxConstraints } from "../rendering/box_constraints";
import { FlexParentData, RenderFlex, type CrossAxisAlignment, type FlexFit, type MainAxisAlignment, type MainAxisSize } from "../rendering/flex";
import type { MouseCursor } from "../rendering/mouse_tracker";
import type { RenderObject } from "../rendering/object";
import {
  CustomPainter, RenderAbsorbPointer, RenderAspectRatio, RenderClipRRect, RenderClipRect, RenderConstrainedBox, RenderCustomPaint,
  RenderDecoratedBox, RenderIgnorePointer, RenderLimitedBox, RenderMouseRegion, RenderOffstage, RenderOpacity, RenderPointerListener,
  RenderRepaintBoundary, RenderTexture, RenderTransform, type DecorationPosition,
} from "../rendering/proxy_box";
import { RenderFractionallySizedBox, RenderPadding, RenderPositionedBox } from "../rendering/shifted_box";
import { RenderStack, StackParentData, type StackFit } from "../rendering/stack";
import {
  LeafRenderObjectView, MultiChildRenderObjectView, ParentDataView, SingleChildRenderObjectView, StatelessView,
  type BuildContext, type View,
} from "./framework";

/** 通用:单子节点参数。 */
export interface SingleChildOptions {
  key?: Key | null;
  child?: View | null;
}

/** 内边距。 */
export class Padding extends SingleChildRenderObjectView {
  readonly padding: EdgeInsets;
  constructor(o: SingleChildOptions & { padding: EdgeInsets }) {
    super(o);
    this.padding = o.padding;
  }
  createRenderObject(): RenderPadding {
    return new RenderPadding(this.padding);
  }
  override updateRenderObject(_c: BuildContext, r: RenderPadding): void {
    r.padding = this.padding;
  }
}

/** 对齐子节点;widthFactor/heightFactor 为 null 时尽量撑满。 */
export class Align extends SingleChildRenderObjectView {
  readonly alignment: Alignment;
  readonly widthFactor: number | null;
  readonly heightFactor: number | null;
  constructor(o: SingleChildOptions & { alignment?: Alignment; widthFactor?: number | null; heightFactor?: number | null } = {}) {
    super(o);
    this.alignment = o.alignment ?? Alignment.center;
    this.widthFactor = o.widthFactor ?? null;
    this.heightFactor = o.heightFactor ?? null;
  }
  createRenderObject(): RenderPositionedBox {
    return new RenderPositionedBox({ alignment: this.alignment, widthFactor: this.widthFactor, heightFactor: this.heightFactor });
  }
  override updateRenderObject(_c: BuildContext, r: RenderPositionedBox): void {
    r.alignment = this.alignment;
    r.widthFactor = this.widthFactor;
    r.heightFactor = this.heightFactor;
  }
}

/** 居中。 */
export class Center extends Align {
  constructor(o: SingleChildOptions & { widthFactor?: number | null; heightFactor?: number | null } = {}) {
    super({ ...o, alignment: Alignment.center });
  }
}

/** 按父尺寸比例约束。 */
export class FractionallySizedBox extends SingleChildRenderObjectView {
  readonly alignment: Alignment;
  readonly widthFactor: number | null;
  readonly heightFactor: number | null;
  constructor(o: SingleChildOptions & { alignment?: Alignment; widthFactor?: number | null; heightFactor?: number | null }) {
    super(o);
    this.alignment = o.alignment ?? Alignment.center;
    this.widthFactor = o.widthFactor ?? null;
    this.heightFactor = o.heightFactor ?? null;
  }
  createRenderObject(): RenderFractionallySizedBox {
    return new RenderFractionallySizedBox({ alignment: this.alignment, widthFactor: this.widthFactor, heightFactor: this.heightFactor });
  }
  override updateRenderObject(_c: BuildContext, r: RenderFractionallySizedBox): void {
    r.alignment = this.alignment;
    r.widthFactor = this.widthFactor;
    r.heightFactor = this.heightFactor;
  }
}

/** 额外约束。 */
export class ConstrainedBox extends SingleChildRenderObjectView {
  readonly constraints: BoxConstraints;
  constructor(o: SingleChildOptions & { constraints: BoxConstraints }) {
    super(o);
    this.constraints = o.constraints;
  }
  createRenderObject(): RenderConstrainedBox {
    return new RenderConstrainedBox(this.constraints);
  }
  override updateRenderObject(_c: BuildContext, r: RenderConstrainedBox): void {
    r.additionalConstraints = this.constraints;
  }
}

/** 固定尺寸盒(未指定的维度跟随子节点)。 */
export class SizedBox extends SingleChildRenderObjectView {
  readonly width: number | null;
  readonly height: number | null;
  constructor(o: SingleChildOptions & { width?: number | null; height?: number | null } = {}) {
    super(o);
    this.width = o.width ?? null;
    this.height = o.height ?? null;
  }
  /** 撑满父约束。 */
  static expand(o: SingleChildOptions = {}): SizedBox {
    return new SizedBox({ ...o, width: Infinity, height: Infinity });
  }
  /** 0×0。 */
  static shrink(o: SingleChildOptions = {}): SizedBox {
    return new SizedBox({ ...o, width: 0, height: 0 });
  }
  /** 正方形。 */
  static square(dimension: number, o: SingleChildOptions = {}): SizedBox {
    return new SizedBox({ ...o, width: dimension, height: dimension });
  }
  #constraints(): BoxConstraints {
    return BoxConstraints.tightFor({ width: this.width, height: this.height });
  }
  createRenderObject(): RenderConstrainedBox {
    return new RenderConstrainedBox(this.#constraints());
  }
  override updateRenderObject(_c: BuildContext, r: RenderConstrainedBox): void {
    r.additionalConstraints = this.#constraints();
  }
}

/** 无界约束下的最大尺寸。 */
export class LimitedBox extends SingleChildRenderObjectView {
  readonly maxWidth: number;
  readonly maxHeight: number;
  constructor(o: SingleChildOptions & { maxWidth?: number; maxHeight?: number }) {
    super(o);
    this.maxWidth = o.maxWidth ?? Infinity;
    this.maxHeight = o.maxHeight ?? Infinity;
  }
  createRenderObject(): RenderLimitedBox {
    return new RenderLimitedBox({ maxWidth: this.maxWidth, maxHeight: this.maxHeight });
  }
  override updateRenderObject(_c: BuildContext, r: RenderLimitedBox): void {
    r.maxWidth = this.maxWidth;
    r.maxHeight = this.maxHeight;
  }
}

/** 宽高比。 */
export class AspectRatio extends SingleChildRenderObjectView {
  readonly aspectRatio: number;
  constructor(o: SingleChildOptions & { aspectRatio: number }) {
    super(o);
    this.aspectRatio = o.aspectRatio;
  }
  createRenderObject(): RenderAspectRatio {
    return new RenderAspectRatio(this.aspectRatio);
  }
  override updateRenderObject(_c: BuildContext, r: RenderAspectRatio): void {
    r.aspectRatio = this.aspectRatio;
  }
}

/** 装饰盒。 */
export class DecoratedBox extends SingleChildRenderObjectView {
  readonly decoration: Decoration;
  readonly position: DecorationPosition;
  constructor(o: SingleChildOptions & { decoration: Decoration; position?: DecorationPosition }) {
    super(o);
    this.decoration = o.decoration;
    this.position = o.position ?? "background";
  }
  createRenderObject(): RenderDecoratedBox {
    return new RenderDecoratedBox(this.decoration, this.position);
  }
  override updateRenderObject(_c: BuildContext, r: RenderDecoratedBox): void {
    r.decoration = this.decoration;
    r.position = this.position;
  }
}

/** 纯色背景盒。 */
export class ColoredBox extends StatelessView {
  readonly color: Color;
  readonly child: View | null;
  constructor(o: SingleChildOptions & { color: Color }) {
    super(o.key);
    this.color = o.color;
    this.child = o.child ?? null;
  }
  build(): View {
    return new DecoratedBox({ decoration: new BoxDecoration({ color: this.color }), child: this.child });
  }
}

/** 透明度。 */
export class Opacity extends SingleChildRenderObjectView {
  readonly opacity: number;
  constructor(o: SingleChildOptions & { opacity: number }) {
    super(o);
    this.opacity = o.opacity;
  }
  createRenderObject(): RenderOpacity {
    return new RenderOpacity(this.opacity);
  }
  override updateRenderObject(_c: BuildContext, r: RenderOpacity): void {
    r.opacity = this.opacity;
  }
}

/** 变换。 */
export class Transform extends SingleChildRenderObjectView {
  readonly transform: Matrix4;
  readonly origin: Offset | null;
  readonly alignment: Alignment | null;
  readonly transformHitTests: boolean;
  constructor(o: SingleChildOptions & { transform: Matrix4; origin?: Offset | null; alignment?: Alignment | null; transformHitTests?: boolean }) {
    super(o);
    this.transform = o.transform;
    this.origin = o.origin ?? null;
    this.alignment = o.alignment ?? null;
    this.transformHitTests = o.transformHitTests ?? true;
  }
  /** 旋转(弧度, 默认绕中心)。 */
  static rotate(o: SingleChildOptions & { angle: number; origin?: Offset | null; alignment?: Alignment }): Transform {
    return new Transform({ ...o, transform: Matrix4.rotationZ(o.angle), alignment: o.alignment ?? Alignment.center });
  }
  /** 平移。 */
  static translate(o: SingleChildOptions & { offset: Offset }): Transform {
    return new Transform({ ...o, transform: Matrix4.translationValues(o.offset.dx, o.offset.dy) });
  }
  /** 缩放(默认绕中心)。 */
  static scale(o: SingleChildOptions & { scale: number; scaleX?: number; scaleY?: number; alignment?: Alignment }): Transform {
    return new Transform({ ...o, transform: Matrix4.diagonal3Values(o.scaleX ?? o.scale, o.scaleY ?? o.scale, 1), alignment: o.alignment ?? Alignment.center });
  }
  createRenderObject(): RenderTransform {
    return new RenderTransform({ transform: this.transform, origin: this.origin, alignment: this.alignment, transformHitTests: this.transformHitTests });
  }
  override updateRenderObject(_c: BuildContext, r: RenderTransform): void {
    r.transform = this.transform;
    r.origin = this.origin;
    r.alignment = this.alignment;
    r.transformHitTests = this.transformHitTests;
  }
}

/** 矩形裁剪。 */
export class ClipRect extends SingleChildRenderObjectView {
  readonly clipBehavior: Clip;
  constructor(o: SingleChildOptions & { clipBehavior?: Clip } = {}) {
    super(o);
    this.clipBehavior = o.clipBehavior ?? "hardEdge";
  }
  createRenderObject(): RenderClipRect {
    return new RenderClipRect(this.clipBehavior);
  }
  override updateRenderObject(_c: BuildContext, r: RenderClipRect): void {
    r.clipBehavior = this.clipBehavior;
  }
}

/** 圆角裁剪。 */
export class ClipRRect extends SingleChildRenderObjectView {
  readonly borderRadius: BorderRadius;
  readonly clipBehavior: Clip;
  constructor(o: SingleChildOptions & { borderRadius?: BorderRadius; clipBehavior?: Clip } = {}) {
    super(o);
    this.borderRadius = o.borderRadius ?? BorderRadius.zero;
    this.clipBehavior = o.clipBehavior ?? "antiAlias";
  }
  createRenderObject(): RenderClipRRect {
    return new RenderClipRRect(this.borderRadius, this.clipBehavior);
  }
  override updateRenderObject(_c: BuildContext, r: RenderClipRRect): void {
    r.borderRadius = this.borderRadius;
    r.clipBehavior = this.clipBehavior;
  }
}

/** 自定义绘制。 */
export class CustomPaint extends SingleChildRenderObjectView {
  readonly painter: CustomPainter | null;
  readonly foregroundPainter: CustomPainter | null;
  readonly size: Size;
  constructor(o: SingleChildOptions & { painter?: CustomPainter | null; foregroundPainter?: CustomPainter | null; size?: Size }) {
    super(o);
    this.painter = o.painter ?? null;
    this.foregroundPainter = o.foregroundPainter ?? null;
    this.size = o.size ?? Size.zero;
  }
  createRenderObject(): RenderCustomPaint {
    return new RenderCustomPaint({ painter: this.painter, foregroundPainter: this.foregroundPainter, preferredSize: this.size });
  }
  override updateRenderObject(_c: BuildContext, r: RenderCustomPaint): void {
    r.painter = this.painter;
    r.foregroundPainter = this.foregroundPainter;
    r.preferredSize = this.size;
  }
}

/** 原始指针事件监听。 */
export class Listener extends SingleChildRenderObjectView {
  readonly onPointerDown: ((e: PointerDownEvent) => void) | null;
  readonly onPointerMove: ((e: PointerMoveEvent) => void) | null;
  readonly onPointerUp: ((e: PointerUpEvent) => void) | null;
  readonly onPointerHover: ((e: PointerHoverEvent) => void) | null;
  readonly onPointerCancel: ((e: PointerCancelEvent) => void) | null;
  readonly onPointerSignal: ((e: PointerSignalEvent) => void) | null;
  readonly behavior: HitTestBehavior;
  constructor(o: SingleChildOptions & {
    onPointerDown?: ((e: PointerDownEvent) => void) | null;
    onPointerMove?: ((e: PointerMoveEvent) => void) | null;
    onPointerUp?: ((e: PointerUpEvent) => void) | null;
    onPointerHover?: ((e: PointerHoverEvent) => void) | null;
    onPointerCancel?: ((e: PointerCancelEvent) => void) | null;
    onPointerSignal?: ((e: PointerSignalEvent) => void) | null;
    behavior?: HitTestBehavior;
  }) {
    super(o);
    this.onPointerDown = o.onPointerDown ?? null;
    this.onPointerMove = o.onPointerMove ?? null;
    this.onPointerUp = o.onPointerUp ?? null;
    this.onPointerHover = o.onPointerHover ?? null;
    this.onPointerCancel = o.onPointerCancel ?? null;
    this.onPointerSignal = o.onPointerSignal ?? null;
    this.behavior = o.behavior ?? "deferToChild";
  }
  createRenderObject(): RenderPointerListener {
    const r = new RenderPointerListener(this.behavior);
    this.updateRenderObject(null, r);
    return r;
  }
  override updateRenderObject(_c: BuildContext | null, r: RenderPointerListener): void {
    r.behavior = this.behavior;
    r.onPointerDown = this.onPointerDown;
    r.onPointerMove = this.onPointerMove;
    r.onPointerUp = this.onPointerUp;
    r.onPointerHover = this.onPointerHover;
    r.onPointerCancel = this.onPointerCancel;
    r.onPointerSignal = this.onPointerSignal;
  }
}

/** 鼠标区域:enter/exit/hover 与光标。 */
export class MouseRegion extends SingleChildRenderObjectView {
  readonly onEnter: ((e: PointerEnterEvent) => void) | null;
  readonly onExit: ((e: PointerExitEvent) => void) | null;
  readonly onHover: ((e: PointerHoverEvent) => void) | null;
  readonly cursor: MouseCursor;
  readonly opaque: boolean;
  constructor(o: SingleChildOptions & {
    onEnter?: ((e: PointerEnterEvent) => void) | null;
    onExit?: ((e: PointerExitEvent) => void) | null;
    onHover?: ((e: PointerHoverEvent) => void) | null;
    cursor?: MouseCursor;
    opaque?: boolean;
  } = {}) {
    super(o);
    this.onEnter = o.onEnter ?? null;
    this.onExit = o.onExit ?? null;
    this.onHover = o.onHover ?? null;
    this.cursor = o.cursor ?? "defer";
    this.opaque = o.opaque ?? true;
  }
  createRenderObject(): RenderMouseRegion {
    const r = new RenderMouseRegion({ cursor: this.cursor, opaque: this.opaque });
    this.updateRenderObject(null, r);
    return r;
  }
  override updateRenderObject(_c: BuildContext | null, r: RenderMouseRegion): void {
    r.onEnter = this.onEnter;
    r.onExit = this.onExit;
    r.onHover = this.onHover;
    r.cursor = this.cursor;
    r.opaque = this.opaque;
  }
}

/** 子树不响应指针。 */
export class IgnorePointer extends SingleChildRenderObjectView {
  readonly ignoring: boolean;
  constructor(o: SingleChildOptions & { ignoring?: boolean } = {}) {
    super(o);
    this.ignoring = o.ignoring ?? true;
  }
  createRenderObject(): RenderIgnorePointer {
    return new RenderIgnorePointer(this.ignoring);
  }
  override updateRenderObject(_c: BuildContext, r: RenderIgnorePointer): void {
    if (r.ignoring !== this.ignoring) r.ignoring = this.ignoring;
  }
}

/** 吸收指针(子树与下层都收不到)。 */
export class AbsorbPointer extends SingleChildRenderObjectView {
  readonly absorbing: boolean;
  constructor(o: SingleChildOptions & { absorbing?: boolean } = {}) {
    super(o);
    this.absorbing = o.absorbing ?? true;
  }
  createRenderObject(): RenderAbsorbPointer {
    return new RenderAbsorbPointer(this.absorbing);
  }
  override updateRenderObject(_c: BuildContext, r: RenderAbsorbPointer): void {
    r.absorbing = this.absorbing;
  }
}

/** 重绘边界。 */
export class RepaintBoundary extends SingleChildRenderObjectView {
  createRenderObject(): RenderRepaintBoundary {
    return new RenderRepaintBoundary();
  }
}

/** 隐藏但保持状态。 */
export class Offstage extends SingleChildRenderObjectView {
  readonly offstage: boolean;
  constructor(o: SingleChildOptions & { offstage?: boolean } = {}) {
    super(o);
    this.offstage = o.offstage ?? true;
  }
  createRenderObject(): RenderOffstage {
    return new RenderOffstage(this.offstage);
  }
  override updateRenderObject(_c: BuildContext, r: RenderOffstage): void {
    r.offstage = this.offstage;
  }
}

/** 外部纹理(扩展注入)。 */
export class Texture extends LeafRenderObjectView {
  readonly textureId: number;
  constructor(o: { key?: Key | null; textureId: number }) {
    super(o.key);
    this.textureId = o.textureId;
  }
  createRenderObject(): RenderTexture {
    return new RenderTexture(this.textureId);
  }
  override updateRenderObject(_c: BuildContext, r: RenderTexture): void {
    r.textureId = this.textureId;
  }
}

// ---------------------------------------------------------------------------
// Flex

/** Flex 参数。 */
export interface FlexOptions {
  key?: Key | null;
  children?: readonly View[];
  mainAxisAlignment?: MainAxisAlignment;
  mainAxisSize?: MainAxisSize;
  crossAxisAlignment?: CrossAxisAlignment;
  verticalDirection?: VerticalDirection;
  textBaseline?: TextBaseline | null;
  clipBehavior?: Clip;
}

/** 一维弹性布局。 */
export class Flex extends MultiChildRenderObjectView {
  readonly direction: Axis;
  readonly mainAxisAlignment: MainAxisAlignment;
  readonly mainAxisSize: MainAxisSize;
  readonly crossAxisAlignment: CrossAxisAlignment;
  readonly verticalDirection: VerticalDirection;
  readonly textBaseline: TextBaseline | null;
  readonly clipBehavior: Clip;
  constructor(o: FlexOptions & { direction: Axis }) {
    super(o);
    this.direction = o.direction;
    this.mainAxisAlignment = o.mainAxisAlignment ?? "start";
    this.mainAxisSize = o.mainAxisSize ?? "max";
    this.crossAxisAlignment = o.crossAxisAlignment ?? "center";
    this.verticalDirection = o.verticalDirection ?? "down";
    this.textBaseline = o.textBaseline ?? null;
    this.clipBehavior = o.clipBehavior ?? "none";
  }
  createRenderObject(): RenderFlex {
    return new RenderFlex({
      direction: this.direction, mainAxisAlignment: this.mainAxisAlignment, mainAxisSize: this.mainAxisSize,
      crossAxisAlignment: this.crossAxisAlignment, verticalDirection: this.verticalDirection, textBaseline: this.textBaseline,
      clipBehavior: this.clipBehavior,
    });
  }
  override updateRenderObject(_c: BuildContext, r: RenderFlex): void {
    r.direction = this.direction;
    r.mainAxisAlignment = this.mainAxisAlignment;
    r.mainAxisSize = this.mainAxisSize;
    r.crossAxisAlignment = this.crossAxisAlignment;
    r.verticalDirection = this.verticalDirection;
    r.textBaseline = this.textBaseline;
    r.clipBehavior = this.clipBehavior;
  }
}

/** 横向排列。 */
export class Row extends Flex {
  constructor(o: FlexOptions = {}) {
    super({ ...o, direction: "horizontal" });
  }
}

/** 纵向排列。 */
export class Column extends Flex {
  constructor(o: FlexOptions = {}) {
    super({ ...o, direction: "vertical" });
  }
}

/** 弹性子节点。 */
export class Flexible extends ParentDataView {
  readonly flex: number;
  readonly fit: FlexFit;
  constructor(o: { key?: Key | null; flex?: number; fit?: FlexFit; child: View }) {
    super(o);
    this.flex = o.flex ?? 1;
    this.fit = o.fit ?? "loose";
  }
  applyParentData(renderObject: RenderObject): void {
    const pd = renderObject.parentData;
    if (!(pd instanceof FlexParentData)) return;
    if (pd.flex !== this.flex || pd.fit !== this.fit) {
      pd.flex = this.flex;
      pd.fit = this.fit;
      renderObject.parent?.markNeedsLayout();
    }
  }
}

/** 填满剩余空间的弹性子节点(fit = tight)。 */
export class Expanded extends Flexible {
  constructor(o: { key?: Key | null; flex?: number; child: View }) {
    super({ ...o, fit: "tight" });
  }
}

/** 占位空白(Expanded + 空盒)。 */
export class Spacer extends StatelessView {
  readonly flex: number;
  constructor(o: { key?: Key | null; flex?: number } = {}) {
    super(o.key);
    this.flex = o.flex ?? 1;
  }
  build(): View {
    return new Expanded({ flex: this.flex, child: SizedBox.shrink() });
  }
}

// ---------------------------------------------------------------------------
// Stack

/** 层叠。 */
export class Stack extends MultiChildRenderObjectView {
  readonly alignment: Alignment;
  readonly fit: StackFit;
  readonly clipBehavior: Clip;
  constructor(o: { key?: Key | null; children?: readonly View[]; alignment?: Alignment; fit?: StackFit; clipBehavior?: Clip } = {}) {
    super(o);
    this.alignment = o.alignment ?? Alignment.topLeft;
    this.fit = o.fit ?? "loose";
    this.clipBehavior = o.clipBehavior ?? "hardEdge";
  }
  createRenderObject(): RenderStack {
    return new RenderStack({ alignment: this.alignment, fit: this.fit, clipBehavior: this.clipBehavior });
  }
  override updateRenderObject(_c: BuildContext, r: RenderStack): void {
    r.alignment = this.alignment;
    r.fit = this.fit;
    r.clipBehavior = this.clipBehavior;
  }
}

/** Stack 中的定位子节点。 */
export class Positioned extends ParentDataView {
  readonly left: number | null;
  readonly top: number | null;
  readonly right: number | null;
  readonly bottom: number | null;
  readonly width: number | null;
  readonly height: number | null;
  constructor(o: { key?: Key | null; left?: number | null; top?: number | null; right?: number | null; bottom?: number | null; width?: number | null; height?: number | null; child: View }) {
    super(o);
    this.left = o.left ?? null;
    this.top = o.top ?? null;
    this.right = o.right ?? null;
    this.bottom = o.bottom ?? null;
    this.width = o.width ?? null;
    this.height = o.height ?? null;
  }
  /** 四边贴合。 */
  static fill(o: { key?: Key | null; left?: number; top?: number; right?: number; bottom?: number; child: View }): Positioned {
    return new Positioned({ ...o, left: o.left ?? 0, top: o.top ?? 0, right: o.right ?? 0, bottom: o.bottom ?? 0 });
  }
  applyParentData(renderObject: RenderObject): void {
    const pd = renderObject.parentData;
    if (!(pd instanceof StackParentData)) return;
    let changed = false;
    if (pd.left !== this.left) { pd.left = this.left; changed = true; }
    if (pd.top !== this.top) { pd.top = this.top; changed = true; }
    if (pd.right !== this.right) { pd.right = this.right; changed = true; }
    if (pd.bottom !== this.bottom) { pd.bottom = this.bottom; changed = true; }
    if (pd.width !== this.width) { pd.width = this.width; changed = true; }
    if (pd.height !== this.height) { pd.height = this.height; changed = true; }
    if (changed) renderObject.parent?.markNeedsLayout();
  }
}

/** 以回调构建(获取下层 context)。 */
export class Builder extends StatelessView {
  readonly builder: (context: BuildContext) => View;
  constructor(o: { key?: Key | null; builder: (context: BuildContext) => View }) {
    super(o.key);
    this.builder = o.builder;
  }
  build(context: BuildContext): View {
    return this.builder(context);
  }
}

/** 携带 key 的透明包装。 */
export class KeyedSubtree extends StatelessView {
  readonly child: View;
  constructor(o: { key?: Key | null; child: View }) {
    super(o.key);
    this.child = o.child;
  }
  build(): View {
    return this.child;
  }
}

/** 空白占位。 */
export function placeholder(): View {
  return SizedBox.shrink();
}
