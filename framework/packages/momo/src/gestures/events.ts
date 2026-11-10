import { Offset } from "../painting/geometry";
import type { Matrix4 } from "../painting/matrix4";

/** 指针设备类型。 */
export type PointerDeviceKind = "mouse" | "trackpad" | "touch" | "stylus";

/** 主键(鼠标左键/触摸)。 */
export const kPrimaryButton = 1;
/** 次键(鼠标右键)。 */
export const kSecondaryButton = 2;
/** 中键。 */
export const kMiddleMouseButton = 4;

/** 指针事件通用字段。 */
export interface PointerEventInit {
  timeStamp?: number;
  pointer?: number;
  kind?: PointerDeviceKind;
  device?: number;
  position?: Offset;
  delta?: Offset;
  buttons?: number;
  down?: boolean;
  /** 触控板惯性滚动阶段。 */
  momentum?: boolean;
}

/**
 * 指针事件基类(位置为全局逻辑像素)。localPosition 由 {@link transformed} 按命中路径变换填充。
 */
export abstract class PointerEvent {
  /** 毫秒。 */
  readonly timeStamp: number;
  readonly pointer: number;
  readonly kind: PointerDeviceKind;
  readonly device: number;
  readonly position: Offset;
  readonly delta: Offset;
  readonly buttons: number;
  readonly down: boolean;
  readonly momentum: boolean;
  /** 局部坐标位置。 */
  readonly localPosition: Offset;
  /** 局部坐标位移。 */
  readonly localDelta: Offset;
  /** 全局 → 局部 的变换(null = 恒等)。 */
  readonly transform: Matrix4 | null;
  /** 未变换的原始事件(本身为原始事件时为 null)。 */
  readonly original: PointerEvent | null;

  constructor(init: PointerEventInit) {
    this.timeStamp = init.timeStamp ?? 0;
    this.pointer = init.pointer ?? 0;
    this.kind = init.kind ?? "touch";
    this.device = init.device ?? 0;
    this.position = init.position ?? Offset.zero;
    this.delta = init.delta ?? Offset.zero;
    this.buttons = init.buttons ?? 0;
    this.down = init.down ?? false;
    this.momentum = init.momentum ?? false;
    this.localPosition = this.position;
    this.localDelta = this.delta;
    this.transform = null;
    this.original = null;
  }

  /** 返回按 transform(全局 → 局部)变换后的副本;transform 为 null 时返回原始事件。 */
  transformed(transform: Matrix4 | null): this {
    const original = this.original ?? this;
    if (transform === null || transform === original.transform) return original as this;
    const copy = Object.create(Object.getPrototypeOf(original) as object) as this;
    Object.assign(copy, original);
    const local = transform.transformPoint(original.position);
    const localEnd = transform.transformPoint(original.position.plus(original.delta));
    Object.assign(copy, {
      localPosition: local,
      localDelta: localEnd.minus(local),
      transform,
      original,
    });
    return copy;
  }

  toString(): string {
    return `${this.constructor.name}(pointer: ${this.pointer}, position: ${this.position})`;
  }
}

/** 设备接入。 */
export class PointerAddedEvent extends PointerEvent {}
/** 设备移除。 */
export class PointerRemovedEvent extends PointerEvent {}
/** 未按下时移动(鼠标悬停)。 */
export class PointerHoverEvent extends PointerEvent {}
/** 按下。 */
export class PointerDownEvent extends PointerEvent {
  constructor(init: PointerEventInit) {
    super({ ...init, down: true });
  }
}
/** 按下后移动。 */
export class PointerMoveEvent extends PointerEvent {
  constructor(init: PointerEventInit) {
    super({ ...init, down: true });
  }
}
/** 抬起。 */
export class PointerUpEvent extends PointerEvent {}
/** 取消(如设备移除时仍按下)。 */
export class PointerCancelEvent extends PointerEvent {}

/** 信号事件(不属于按下序列, 如滚轮)。 */
export abstract class PointerSignalEvent extends PointerEvent {}

/** 滚轮/触控板滚动(scrollDelta 为逻辑像素, 向下滚为正)。 */
export class PointerScrollEvent extends PointerSignalEvent {
  readonly scrollDelta: Offset;
  constructor(init: PointerEventInit & { scrollDelta: Offset }) {
    super(init);
    this.scrollDelta = init.scrollDelta;
  }
}

/** 鼠标进入区域(由 MouseTracker 合成)。 */
export class PointerEnterEvent extends PointerEvent {}
/** 鼠标离开区域(由 MouseTracker 合成)。 */
export class PointerExitEvent extends PointerEvent {}

/** 是否为精确指针(鼠标/触控板)。 */
export function isPrecisePointer(kind: PointerDeviceKind): boolean {
  return kind === "mouse" || kind === "trackpad";
}
