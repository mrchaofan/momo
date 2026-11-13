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
  /** 屏幕位置(逻辑点, 主屏左上原点;与 Widget.bounds 同一坐标系)。缺省为零点(来源未提供)。 */
  screenPosition?: Offset;
  /** 宿主的按下序列 id(0 = 不在序列内)。 */
  sequenceId?: number;
  /** 事件来自的视图(窗口)id;position 是该视图里的坐标。缺省 0(隐式视图)。 */
  viewId?: number;
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
  /**
   * 指针的屏幕位置(逻辑点, 主屏左上原点, 与 Widget.bounds 同一坐标系)。
   * 不随窗口移动而变化: 跨窗口交互(如把标签拖出成窗口、拖动窗口本身)用它计算。
   */
  readonly screenPosition: Offset;
  /**
   * 宿主的按下序列 id(首个按下到最后一个抬起相同, > 0;不在序列内为 0)。调用依赖进行中序列的窗口能力时
   * 回送给宿主校验: Widget.startDrag(业务层的拖动区域)、PointerCapture.moveToView。
   */
  readonly sequenceId: number;
  /**
   * 事件来自的视图(窗口)id(同 Flutter PointerEvent.viewId)。position 是该视图里的坐标;命中测试、光标、
   * 平台视图的投递都按它找窗口——多窗口共用一套指针状态, 不存在"当前视图"这种全局状态。
   */
  readonly viewId: number;
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
    this.screenPosition = init.screenPosition ?? Offset.zero;
    this.sequenceId = init.sequenceId ?? 0;
    this.viewId = init.viewId ?? 0;
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

/**
 * 触控板滚动手势阶段: "none" 普通滚轮(每次命中测试);"start" 手势开始(命中测试并锁定接收者);
 * "update" 手势中与惯性阶段(沿用锁定);"end" 手势/惯性结束(沿用后解除)。
 */
export type ScrollPhase = "none" | "start" | "update" | "end";

/** 滚轮/触控板滚动(scrollDelta 为逻辑像素, 向下滚为正;手势开始/结束时可为零)。 */
export class PointerScrollEvent extends PointerSignalEvent {
  readonly scrollDelta: Offset;
  readonly scrollPhase: ScrollPhase;
  constructor(init: PointerEventInit & { scrollDelta: Offset; scrollPhase?: ScrollPhase }) {
    super(init);
    this.scrollDelta = init.scrollDelta;
    this.scrollPhase = init.scrollPhase ?? "none";
  }
}

/** 平台手势种类(macOS 触控板等;docs/05 §9 "输入")。 */
export type PointerGestureKind =
  "magnify" | "rotate" | "smartMagnify" | "swipe" | "quickLook" | "tabletPoint" | "begin" | "end";

/**
 * 带位置的平台手势(信号事件, 不属于按下序列): 触控板捏合/旋转/轻扫/智能缩放、查询、手写板点…
 * 与滚动一样命中测试(触控板手势按 phase 锁定), 经 PointerSignalResolver 由最先登记者处理;
 * 落在平台视图上的交给原生视图。value: magnify 为缩放增量(1 + value 为比例), rotate 为角度(度, 逆时针为正),
 * tabletPoint 为压力;swipe 的方向在 swipeDelta(视口方向, 分量为 -1/0/1)。
 */
export class PointerGestureEvent extends PointerSignalEvent {
  readonly gesture: PointerGestureKind;
  readonly phase: ScrollPhase;
  readonly value: number;
  readonly swipeDelta: Offset;
  constructor(init: PointerEventInit & { gesture: PointerGestureKind; phase?: ScrollPhase; value?: number; swipeDelta?: Offset }) {
    super(init);
    this.gesture = init.gesture;
    this.phase = init.phase ?? "none";
    this.value = init.value ?? 0;
    this.swipeDelta = init.swipeDelta ?? Offset.zero;
  }
}

/**
 * 按下序列内的压力变化(macOS Force Touch)。沿按下时的命中路径投递(与拖动相同, down = true)。
 * pressure 0..1;stage: 0 释放, 1 点按, 2 用力点按。
 */
export class PointerPressureEvent extends PointerEvent {
  readonly pressure: number;
  readonly stage: number;
  constructor(init: PointerEventInit & { pressure: number; stage: number }) {
    super({ ...init, down: true });
    this.pressure = init.pressure;
    this.stage = init.stage;
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
