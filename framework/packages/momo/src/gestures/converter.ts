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
  nativeEventId: 12,
  signalPhase: 13,
  gestureKind: 14,
  gestureValue: 15,
  gestureStage: 16,
  stride: 17,
} as const;

/** 指针包 phase 取值。 */
export const PointerPhase = { add: 0, down: 1, move: 2, up: 3, remove: 4, hover: 5, scroll: 6, gesture: 7 } as const;

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
}

/**
 * 把引擎的 Float64Array 指针包解码为 PointerEvent(物理像素 → 逻辑像素), 维护每设备状态,
 * 为每次按下序列分配唯一 pointer id, 必要时补发 add/cancel。
 */
export class PointerEventConverter {
  #devices = new Map<number, DeviceState>();
  #nextPointer = 0;

  /** 解码 count 个事件(data 仅在回调期间有效, 本函数立即读取)。 */
  expand(data: Float64Array, count: number, devicePixelRatio: number, stride: number = PointerDataField.stride): PointerEvent[] {
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
        state = { added: false, down: false, pointer: 0, last: position };
        this.#devices.set(device, state);
      }
      const delta = position.minus(state.last);
      const base = { timeStamp, kind, device, position, momentum, screenPosition };
      // 原生事件 id 给直接对应该原生事件的指针事件(含 add/remove: 它们引起的进入/离开取其位置);
      // 补发的 cancel 不带(它不对应原生事件)。
      const native = { ...base, nativeEventId: stride > PointerDataField.nativeEventId ? data[b + PointerDataField.nativeEventId]! : 0 };
      if (!state.added && phase !== PointerPhase.add && phase !== PointerPhase.remove) {
        state.added = true;
        out.push(new PointerAddedEvent({ ...native, pointer: state.pointer }));
      }
      switch (phase) {
        case PointerPhase.add:
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
        case PointerPhase.remove:
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
    }
    return out;
  }

  /** 清空设备状态(测试用)。 */
  reset(): void {
    this.#devices.clear();
  }
}
