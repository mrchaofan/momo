import type { Picture, SceneBuilder } from "momo:ui";
import { BorderRadius } from "../painting/borders";
import { Offset, Rect } from "../painting/geometry";
import { Matrix4 } from "../painting/matrix4";

/**
 * 合成层(engine LayerTree 的 TS 镜像)。每帧经 {@link ContainerLayer.addToScene} 一一映射到 SceneBuilder:
 * OffsetLayer→pushOffset、TransformLayer→pushTransform、ClipRectLayer→pushClipRect、
 * ClipRRectLayer→pushClipRRect、OpacityLayer→pushOpacity、PictureLayer→addPicture、TextureLayer→addTexture、
 * PlatformViewLayer→addPlatformView。
 */
export abstract class Layer {
  parent: ContainerLayer | null = null;

  /** 从父层移除。 */
  remove(): void {
    this.parent?.removeChild(this);
  }

  /** 写入 SceneBuilder。 */
  abstract addToScene(builder: SceneBuilder): void;

  /** 调试描述(一行)。 */
  describe(): string {
    return this.constructor.name;
  }

  /** 调试:子层。 */
  debugChildren(): readonly Layer[] {
    return [];
  }

  /** 调试:树形文本。 */
  toStringDeep(indent = ""): string {
    let s = indent + this.describe() + "\n";
    for (const c of this.debugChildren()) s += c.toStringDeep(indent + "  ");
    return s;
  }
}

/** 含子层的层。 */
export class ContainerLayer extends Layer {
  readonly children: Layer[] = [];

  append(child: Layer): void {
    if (child.parent !== null) child.remove();
    child.parent = this;
    this.children.push(child);
  }

  removeChild(child: Layer): void {
    const i = this.children.indexOf(child);
    if (i >= 0) {
      this.children.splice(i, 1);
      child.parent = null;
    }
  }

  removeAllChildren(): void {
    for (const c of this.children) c.parent = null;
    this.children.length = 0;
  }

  addChildrenToScene(builder: SceneBuilder): void {
    const children = this.children;
    for (let i = 0; i < children.length; i++) children[i]!.addToScene(builder);
  }

  addToScene(builder: SceneBuilder): void {
    this.addChildrenToScene(builder);
  }

  override debugChildren(): readonly Layer[] {
    return this.children;
  }
}

/** 平移层(RepaintBoundary 的层)。 */
export class OffsetLayer extends ContainerLayer {
  offset: Offset;
  constructor(offset: Offset = Offset.zero) {
    super();
    this.offset = offset;
  }
  override addToScene(builder: SceneBuilder): void {
    builder.pushOffset(this.offset.dx, this.offset.dy);
    this.addChildrenToScene(builder);
    builder.pop();
  }
  override describe(): string {
    return `OffsetLayer(${this.offset.dx}, ${this.offset.dy})`;
  }
}

const sceneMatrix = new Matrix4();

/** 变换层。 */
export class TransformLayer extends OffsetLayer {
  transform: Matrix4;
  constructor(transform: Matrix4 = new Matrix4(), offset: Offset = Offset.zero) {
    super(offset);
    this.transform = transform;
  }
  override addToScene(builder: SceneBuilder): void {
    let m = this.transform;
    if (this.offset.dx !== 0 || this.offset.dy !== 0) {
      m = sceneMatrix.setIdentity().translate(this.offset.dx, this.offset.dy).multiply(this.transform);
    }
    builder.pushTransform(m.storage);
    this.addChildrenToScene(builder);
    builder.pop();
  }
  override describe(): string {
    return `TransformLayer([${Array.from(this.transform.storage).join(",")}])`;
  }
}

/** 矩形裁剪层。 */
export class ClipRectLayer extends ContainerLayer {
  clipRect: Rect;
  constructor(clipRect: Rect = Rect.zero) {
    super();
    this.clipRect = clipRect;
  }
  override addToScene(builder: SceneBuilder): void {
    const r = this.clipRect;
    builder.pushClipRect(r.left, r.top, r.width, r.height);
    this.addChildrenToScene(builder);
    builder.pop();
  }
  override describe(): string {
    return `ClipRectLayer(${this.clipRect})`;
  }
}

/**
 * 圆角矩形裁剪层。SceneBuilder.pushClipRRect 只支持单一半径:非均匀圆角取四角最小值(契约缺口, 见汇报)。
 */
export class ClipRRectLayer extends ContainerLayer {
  clipRect: Rect;
  borderRadius: BorderRadius;
  constructor(clipRect: Rect = Rect.zero, borderRadius: BorderRadius = BorderRadius.zero) {
    super();
    this.clipRect = clipRect;
    this.borderRadius = borderRadius;
  }
  override addToScene(builder: SceneBuilder): void {
    const r = this.clipRect;
    const br = this.borderRadius.clampedFor(r);
    const radius = Math.min(br.topLeft, br.topRight, br.bottomRight, br.bottomLeft);
    builder.pushClipRRect(r.left, r.top, r.width, r.height, radius);
    this.addChildrenToScene(builder);
    builder.pop();
  }
  override describe(): string {
    return `ClipRRectLayer(${this.clipRect}, r=${this.borderRadius.topLeft})`;
  }
}

/** 透明度层(alpha 0..255, 可带平移)。 */
export class OpacityLayer extends ContainerLayer {
  alpha: number;
  offset: Offset;
  constructor(alpha = 255, offset: Offset = Offset.zero) {
    super();
    this.alpha = alpha;
    this.offset = offset;
  }
  override addToScene(builder: SceneBuilder): void {
    const shifted = this.offset.dx !== 0 || this.offset.dy !== 0;
    if (shifted) builder.pushOffset(this.offset.dx, this.offset.dy);
    builder.pushOpacity(this.alpha);
    this.addChildrenToScene(builder);
    builder.pop();
    if (shifted) builder.pop();
  }
  override describe(): string {
    return `OpacityLayer(alpha=${this.alpha}, offset=${this.offset.dx},${this.offset.dy})`;
  }
}

/** 图片(绘制命令)层。 */
export class PictureLayer extends Layer {
  picture: Picture | null = null;
  constructor(readonly canvasBounds: Rect) {
    super();
  }
  addToScene(builder: SceneBuilder): void {
    if (this.picture !== null) builder.addPicture(0, 0, this.picture);
  }
  override describe(): string {
    return `PictureLayer(ops=${this.picture?.opCount ?? 0})`;
  }
}

/** 外部纹理层。 */
export class TextureLayer extends Layer {
  constructor(public rect: Rect, public textureId: number) {
    super();
  }
  addToScene(builder: SceneBuilder): void {
    builder.addTexture(this.textureId, this.rect.left, this.rect.top, this.rect.width, this.rect.height);
  }
  override describe(): string {
    return `TextureLayer(id=${this.textureId}, ${this.rect})`;
  }
}

/** 平台视图层(系统原生视图占位, docs/05 §9)。之后添加的层画在该视图之上。 */
export class PlatformViewLayer extends Layer {
  constructor(public rect: Rect, public viewId: number) {
    super();
  }
  addToScene(builder: SceneBuilder): void {
    builder.addPlatformView(this.viewId, this.rect.left, this.rect.top, this.rect.width, this.rect.height);
  }
  override describe(): string {
    return `PlatformViewLayer(id=${this.viewId}, ${this.rect})`;
  }
}
