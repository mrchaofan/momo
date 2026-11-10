import type { KeyData } from "momo:ui";
import { reportError } from "../foundation/assert";
import { LogicalKeyboardKey, PhysicalKeyboardKey } from "./keyboard_key";

/** 修饰键位图(与 momo:ui KeyData.modifiers 一致)。 */
export const KeyModifier = {
  shift: 1,
  control: 2,
  alt: 4,
  meta: 8,
  capsLock: 16,
} as const;

/** 键盘事件处理结果。 */
export type KeyEventResult = "handled" | "ignored" | "skipRemainingHandlers";

/** 键盘事件基类。 */
export abstract class KeyEvent {
  constructor(
    readonly physicalKey: PhysicalKeyboardKey,
    readonly logicalKey: LogicalKeyboardKey,
    readonly character: string | null,
    /** 毫秒。 */
    readonly timeStamp: number,
    /** 修饰键位图(事件发生时)。 */
    readonly modifiers: number = 0,
    readonly synthesized: boolean = false,
  ) {}
  toString(): string {
    return `${this.constructor.name}(${this.logicalKey.debugName ?? this.logicalKey.keyId}, char: ${JSON.stringify(this.character)})`;
  }
}

/** 按下。 */
export class KeyDownEvent extends KeyEvent {}
/** 抬起。 */
export class KeyUpEvent extends KeyEvent {}
/** 长按重复。 */
export class KeyRepeatEvent extends KeyEvent {}

/** 键盘事件处理器;返回 true 表示已处理。 */
export type KeyEventHandler = (event: KeyEvent) => boolean;

/** 硬件键盘状态与事件分发(对标 Flutter HardwareKeyboard)。 */
export class HardwareKeyboard {
  static #instance: HardwareKeyboard | null = null;
  static get instance(): HardwareKeyboard {
    if (HardwareKeyboard.#instance === null) HardwareKeyboard.#instance = new HardwareKeyboard();
    return HardwareKeyboard.#instance;
  }

  #pressed = new Map<PhysicalKeyboardKey, LogicalKeyboardKey>();
  #logicalPressed = new Set<LogicalKeyboardKey>();
  #modifiers = 0;
  #handlers: KeyEventHandler[] = [];

  private constructor() {}

  /** 当前按下的物理键。 */
  get physicalKeysPressed(): ReadonlySet<PhysicalKeyboardKey> {
    return new Set(this.#pressed.keys());
  }
  /** 当前按下的逻辑键。 */
  get logicalKeysPressed(): ReadonlySet<LogicalKeyboardKey> {
    return this.#logicalPressed;
  }
  /** 物理键当前对应的逻辑键(未按下为 null)。 */
  lookUpLayout(physical: PhysicalKeyboardKey): LogicalKeyboardKey | null {
    return this.#pressed.get(physical) ?? null;
  }
  /** 最近一次 KeyData 的修饰键位图。 */
  get modifiers(): number {
    return this.#modifiers;
  }
  get isShiftPressed(): boolean {
    return (this.#modifiers & KeyModifier.shift) !== 0;
  }
  get isControlPressed(): boolean {
    return (this.#modifiers & KeyModifier.control) !== 0;
  }
  get isAltPressed(): boolean {
    return (this.#modifiers & KeyModifier.alt) !== 0;
  }
  get isMetaPressed(): boolean {
    return (this.#modifiers & KeyModifier.meta) !== 0;
  }

  addHandler(handler: KeyEventHandler): void {
    this.#handlers.push(handler);
  }
  removeHandler(handler: KeyEventHandler): void {
    const i = this.#handlers.indexOf(handler);
    if (i >= 0) this.#handlers.splice(i, 1);
  }

  /** 把引擎 KeyData 转换为 KeyEvent(不改状态)。 */
  static convert(data: KeyData): KeyEvent {
    const physical = PhysicalKeyboardKey.findKeyByKeyId(data.physicalKey);
    const logical = LogicalKeyboardKey.findKeyByKeyId(data.logicalKey);
    const character = data.character === "" ? null : data.character;
    const ts = data.timeStampUs / 1000;
    switch (data.type) {
      case "down":
        return new KeyDownEvent(physical, logical, character, ts, data.modifiers);
      case "up":
        return new KeyUpEvent(physical, logical, null, ts, data.modifiers);
      default:
        return new KeyRepeatEvent(physical, logical, character, ts, data.modifiers);
    }
  }

  /** 处理一条引擎 KeyData:更新状态并分发给全部 handler;任一返回 true 即视为已处理。 */
  handleKeyData(data: KeyData): boolean {
    const event = HardwareKeyboard.convert(data);
    this.#modifiers = data.modifiers;
    if (event instanceof KeyDownEvent || event instanceof KeyRepeatEvent) {
      const old = this.#pressed.get(event.physicalKey);
      if (old !== undefined) this.#logicalPressed.delete(old);
      this.#pressed.set(event.physicalKey, event.logicalKey);
      this.#logicalPressed.add(event.logicalKey);
    } else {
      const old = this.#pressed.get(event.physicalKey);
      this.#pressed.delete(event.physicalKey);
      if (old !== undefined) this.#logicalPressed.delete(old);
      this.#logicalPressed.delete(event.logicalKey);
    }
    return this.dispatch(event);
  }

  /** 分发事件给 handler(全部调用)。 */
  dispatch(event: KeyEvent): boolean {
    let handled = false;
    for (const h of this.#handlers.slice()) {
      try {
        if (h(event)) handled = true;
      } catch (e) {
        reportError(e, "while dispatching a key event");
      }
    }
    return handled;
  }

  /** 清空按下状态(如窗口失焦)。 */
  clearState(): void {
    this.#pressed.clear();
    this.#logicalPressed.clear();
    this.#modifiers = 0;
  }

  /** 测试用:清空状态与 handler。 */
  debugReset(): void {
    this.clearState();
    this.#handlers = [];
  }
}
