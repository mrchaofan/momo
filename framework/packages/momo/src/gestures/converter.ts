import { Offset } from "../painting/geometry";
import {
  PointerAddedEvent, PointerCancelEvent, PointerDownEvent, PointerHoverEvent, PointerMoveEvent, PointerRemovedEvent,
  PointerGestureEvent, PointerPressureEvent, PointerScrollEvent, PointerUpEvent, kPrimaryButton, type PointerDeviceKind,
  type PointerEvent, type PointerGestureKind, type ScrollPhase,
} from "./events";

/** 指针包字段下标(momo:ui POINTER_DATA_STRIDE = 17, 见 momo-ui.d.ts)。 */
export const PointerDataField = {
  timeStampUs: 0,
  kind: 1,
  phase: 2,
  device: 3,
  physicalX: 4,
  physicalY: 5,
  buttons: 6,
  scrollDeltaX: 7,
  scrollDeltaY: 8,
  momentum: 9,
  screenX: 10,
  screenY: 11,
  sequenceId: 12,
  signalPhase: 13,
  gestureKind: 14,
  gestureValue: 15,
  gestureStage: 16,
  stride: 17,
} as const;

/** 指针包 phase 取值。remove 只表示离开;cancel 表示进行中的按下序列被取消(如接收窗口关闭)。 */
export const PointerPhase = { add: 0, down: 1, move: 2, up: 3, remove: 4, hover: 5, scroll: 6, gesture: 7, cancel: 8 } as const;

const KINDS: readonly PointerDeviceKind[] = ["mouse", "trackpad", "touch", "stylus"];
const SCROLL_PHASES: readonly ScrollPhase[] = ["none", "start", "update", "end"];
/** gestureKind 字段(下标)→ 手势种类;6(pressure)单独成为 PointerPressureEvent。 */
const GESTURES: readonly (PointerGestureKind | "pressure" | null)[] =
  [null, "magnify", "rotate", "smartMagnify", "swipe", "quickLook", "pressure", "tabletPoint", "begin", "end"];

interface DeviceState {
  added: boolean;
  down: boolean;
  pointer: number;
  last: Offset;
  /** 最近一个事件来自的视图(坐标系随视图而不同)。 */
  viewId: number;
  lastScreen: Offset;
  /** 进行中按下序列的宿主 id。 */
  sequenceId: number;
}

/** 进行中的按下序列(GestureBinding.capturePointer 用)。 */
export interface ActivePointer {
  readonly pointer: number;
  readonly device: number;
  readonly viewId: number;
  readonly position: Offset;
  readonly screenPosition: Offset;
  /** 宿主的按下序列 id(PointerCapture.moveToView 回送)。 */
  readonly sequenceId: number;
}

/**
 * 把引擎的 Float64Array 指针包解码为 PointerEvent(物理像素 → 逻辑像素), 维护每设备状态,
 * 为每次按下序列分配唯一 pointer id, 必要时补发 add/cancel。
 */
export class PointerEventConverter {
  #devices = new Map<number, DeviceState>();
  #nextPointer = 0;

  /**
   * 解码 count 个事件(data 仅在回调期间有效, 本函数立即读取)。viewId: 事件来自的视图, 记在每个事件上。
   * 设备状态(按下、指针 id)在视图之间共享: 宿主把进行中的序列改交另一个窗口(PointerCapture.moveToView)后,
   * 同一个指针继续, 只是坐标换成新视图的(位移按新视图重新起算)。
   */
  expand(data: Float64Array, count: number, devicePixelRatio: number, viewId = 0, stride: number = PointerDataField.stride): PointerEvent[] {
    const out: PointerEvent[] = [];
    for (let i = 0; i < count; i++) {
      const b = i * stride;
      const timeStamp = data[b + PointerDataField.timeStampUs]! / 1000;
      const kind = KINDS[data[b + PointerDataField.kind]! | 0] ?? "mouse";
      const phase = data[b + PointerDataField.phase]! | 0;
      const device = data[b + PointerDataField.device]! | 0;
      const position = new Offset(data[b + PointerDataField.physicalX]! / devicePixelRatio, data[b + PointerDataField.physicalY]! / devicePixelRatio);
      let buttons = data[b + PointerDataField.buttons]! | 0;
      const momentum = data[b + PointerDataField.momentum] === 1;
      const screenPosition = new Offset(data[b + PointerDataField.screenX]!, data[b + PointerDataField.screenY]!);
      let state = this.#devices.get(device);
      if (state === undefined) {
        state = { added: false, down: false, pointer: 0, last: position, viewId, lastScreen: screenPosition, sequenceId: 0 };
        this.#devices.set(device, state);
      }
      if (state.viewId !== viewId) {
        state.viewId = viewId;
        state.last = position;  // 换了视图(坐标系): 位移重新起算
      }
      const delta = position.minus(state.last);
      const base = { timeStamp, kind, device, position, momentum, screenPosition, viewId };
      const sequenceId = stride > PointerDataField.sequenceId ? data[b + PointerDataField.sequenceId]! : 0;
      if (sequenceId !== 0) state.sequenceId = sequenceId;
      const native = { ...base, sequenceId };
      if (!state.added && phase !== PointerPhase.add && phase !== PointerPhase.remove) {
        state.added = true;
        out.push(new PointerAddedEvent({ ...native, pointer: state.pointer }));
      }
      switch (phase) {
        case PointerPhase.add:
          // 已在别的视图接入(按下序列转交过来的新窗口首次上报): 同一设备不重复接入。
          if (state.added && state.down) break;
          state.added = true;
          out.push(new PointerAddedEvent({ ...native, pointer: state.pointer }));
          break;
        case PointerPhase.down:
          if (state.down) {
            out.push(new PointerCancelEvent({ ...base, pointer: state.pointer }));
          }
          if (buttons === 0) buttons = kPrimaryButton;
          state.down = true;
          state.pointer = ++this.#nextPointer;
          out.push(new PointerDownEvent({ ...native, pointer: state.pointer, buttons }));
          break;
        case PointerPhase.move:
        case PointerPhase.hover:
          if (state.down) {
            if (buttons === 0) buttons = kPrimaryButton;
            out.push(new PointerMoveEvent({ ...native, pointer: state.pointer, buttons, delta }));
          } else {
            out.push(new PointerHoverEvent({ ...native, pointer: state.pointer, buttons, delta }));
          }
          break;
        case PointerPhase.up:
          if (state.down) {
            state.down = false;
            out.push(new PointerUpEvent({ ...native, pointer: state.pointer, buttons: 0 }));
          }
          break;
        case PointerPhase.cancel:
          if (state.down) {
            state.down = false;
            out.push(new PointerCancelEvent({ ...base, pointer: state.pointer }));
          }
          break;
        case PointerPhase.remove:
          // 宿主不在按下中报离开(序列的接收方固定到抬起);不一致时按取消处理, 不让按下状态卡住。
          if (state.down) {
            state.down = false;
            out.push(new PointerCancelEvent({ ...base, pointer: state.pointer }));
          }
          if (state.added) out.push(new PointerRemovedEvent({ ...native, pointer: state.pointer }));
          this.#devices.delete(device);
          break;
        case PointerPhase.scroll: {
          const scrollDelta = new Offset(data[b + PointerDataField.scrollDeltaX]! / devicePixelRatio, data[b + PointerDataField.scrollDeltaY]! / devicePixelRatio);
          const scrollPhase = stride > PointerDataField.signalPhase ? SCROLL_PHASES[data[b + PointerDataField.signalPhase]! | 0] ?? "none" : "none";
          out.push(new PointerScrollEvent({ ...native, pointer: state.pointer, buttons, scrollDelta, scrollPhase }));
          break;
        }
        case PointerPhase.gesture: {
          const gesture = stride > PointerDataField.gestureKind ? GESTURES[data[b + PointerDataField.gestureKind]! | 0] ?? null : null;
          const value = data[b + PointerDataField.gestureValue] ?? 0;
          if (gesture === "pressure") {
            // 只属于按下序列: 没有进行中的按下时丢弃(宿主在按下之外不会产生压力事件)。
            if (state.down) {
              const stage = data[b + PointerDataField.gestureStage]! | 0;
              out.push(new PointerPressureEvent({ ...native, pointer: state.pointer, buttons, pressure: value, stage }));
            }
          } else if (gesture !== null) {
            const phase = SCROLL_PHASES[data[b + PointerDataField.signalPhase]! | 0] ?? "none";
            const swipeDelta = new Offset(data[b + PointerDataField.scrollDeltaX]!, data[b + PointerDataField.scrollDeltaY]!);
            out.push(new PointerGestureEvent({ ...native, pointer: state.pointer, buttons, gesture, phase, value, swipeDelta }));
          }
          break;
        }
        default:
          break;
      }
      state.last = position;
      state.lastScreen = screenPosition;
    }
    return out;
  }

  /** 设备 device(缺省任一设备)进行中的按下序列;没有按下时为 null。 */
  activePointer(device?: number): ActivePointer | null {
    for (const [d, s] of this.#devices) {
      if (s.down && (device === undefined || d === device)) {
        return { pointer: s.pointer, device: d, viewId: s.viewId, position: s.last, screenPosition: s.lastScreen, sequenceId: s.sequenceId };
      }
    }
    return null;
  }

  /** 清空设备状态(测试用)。 */
  reset(): void {
    this.#devices.clear();
  }
}
