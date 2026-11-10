// 键码常量。物理键 = USB HID usage(page << 16 | usage);逻辑键:可打印字符 = Unicode 码点(小写),
// 特殊键 = 0x1_0000_0000 + HID usage(与 momo:ui KeyData 一致);左右合成键(shift/control/alt/meta)位于 0x2_0000_0000 平面。

const SPECIAL_PLANE = 0x1_0000_0000;
const SYNONYM_PLANE = 0x2_0000_0000;
const HID_PAGE = 0x0007_0000;

const logicalRegistry = new Map<number, LogicalKeyboardKey>();
const physicalRegistry = new Map<number, PhysicalKeyboardKey>();

/** 逻辑键(受键盘布局影响的"含义")。实例按 keyId 驻留(同 id 同对象)。 */
export class LogicalKeyboardKey {
  /** 逻辑键 id。 */
  readonly keyId: number;
  /** 调试名(未知键为 null)。 */
  readonly debugName: string | null;

  private constructor(keyId: number, debugName: string | null) {
    this.keyId = keyId;
    this.debugName = debugName;
  }

  /** 按 id 查找(未知 id 会新建并驻留)。 */
  static findKeyByKeyId(keyId: number): LogicalKeyboardKey {
    let k = logicalRegistry.get(keyId);
    if (k === undefined) {
      k = new LogicalKeyboardKey(keyId, null);
      logicalRegistry.set(keyId, k);
    }
    return k;
  }

  /** @internal 定义常量。 */
  static define(keyId: number, debugName: string): LogicalKeyboardKey {
    const k = new LogicalKeyboardKey(keyId, debugName);
    logicalRegistry.set(keyId, k);
    return k;
  }

  /** 由字符得到逻辑键(取首个码点并转小写)。 */
  static fromCharacter(ch: string): LogicalKeyboardKey {
    const cp = ch.toLowerCase().codePointAt(0) ?? 0;
    return LogicalKeyboardKey.findKeyByKeyId(cp);
  }

  equals(other: LogicalKeyboardKey | null | undefined): boolean {
    return other != null && other.keyId === this.keyId;
  }

  /** 是否为可打印字符键。 */
  get isPrintable(): boolean {
    return this.keyId < SPECIAL_PLANE;
  }

  /** 可打印键的字符(小写);否则 null。 */
  get keyLabel(): string | null {
    return this.isPrintable ? String.fromCodePoint(this.keyId) : null;
  }

  /** 左右修饰键对应的合成键(如 shiftLeft → shift), 否则空数组。 */
  get synonyms(): LogicalKeyboardKey[] {
    const s = synonymOf.get(this.keyId);
    return s === undefined ? [] : [s];
  }

  /** 把集合中的左右修饰键折叠为合成键。 */
  static collapseSynonyms(keys: Iterable<LogicalKeyboardKey>): Set<LogicalKeyboardKey> {
    const out = new Set<LogicalKeyboardKey>();
    for (const k of keys) out.add(synonymOf.get(k.keyId) ?? k);
    return out;
  }

  /** key 是否属于合成键 group(group 可为合成键或具体键)。 */
  static matches(group: LogicalKeyboardKey, key: LogicalKeyboardKey): boolean {
    return group === key || synonymOf.get(key.keyId) === group;
  }

  static isShift(key: LogicalKeyboardKey): boolean {
    return LogicalKeyboardKey.matches(LogicalKeyboardKey.shift, key);
  }
  static isControl(key: LogicalKeyboardKey): boolean {
    return LogicalKeyboardKey.matches(LogicalKeyboardKey.control, key);
  }
  static isAlt(key: LogicalKeyboardKey): boolean {
    return LogicalKeyboardKey.matches(LogicalKeyboardKey.alt, key);
  }
  static isMeta(key: LogicalKeyboardKey): boolean {
    return LogicalKeyboardKey.matches(LogicalKeyboardKey.meta, key);
  }
  /** 是否为修饰键(含左右与合成键、CapsLock)。 */
  static isModifier(key: LogicalKeyboardKey): boolean {
    return synonymOf.has(key.keyId) || key.keyId >= SYNONYM_PLANE || key === LogicalKeyboardKey.capsLock;
  }

  toString(): string {
    return `LogicalKeyboardKey#0x${this.keyId.toString(16)}(${this.debugName ?? "unknown"})`;
  }

  // ---- 可打印 ----
  static readonly space = LogicalKeyboardKey.define(0x20, "Space");
  static readonly quote = LogicalKeyboardKey.define(0x27, "Quote");
  static readonly comma = LogicalKeyboardKey.define(0x2c, "Comma");
  static readonly minus = LogicalKeyboardKey.define(0x2d, "Minus");
  static readonly period = LogicalKeyboardKey.define(0x2e, "Period");
  static readonly slash = LogicalKeyboardKey.define(0x2f, "Slash");
  static readonly digit0 = LogicalKeyboardKey.define(0x30, "Digit 0");
  static readonly digit1 = LogicalKeyboardKey.define(0x31, "Digit 1");
  static readonly digit2 = LogicalKeyboardKey.define(0x32, "Digit 2");
  static readonly digit3 = LogicalKeyboardKey.define(0x33, "Digit 3");
  static readonly digit4 = LogicalKeyboardKey.define(0x34, "Digit 4");
  static readonly digit5 = LogicalKeyboardKey.define(0x35, "Digit 5");
  static readonly digit6 = LogicalKeyboardKey.define(0x36, "Digit 6");
  static readonly digit7 = LogicalKeyboardKey.define(0x37, "Digit 7");
  static readonly digit8 = LogicalKeyboardKey.define(0x38, "Digit 8");
  static readonly digit9 = LogicalKeyboardKey.define(0x39, "Digit 9");
  static readonly semicolon = LogicalKeyboardKey.define(0x3b, "Semicolon");
  static readonly equal = LogicalKeyboardKey.define(0x3d, "Equal");
  static readonly bracketLeft = LogicalKeyboardKey.define(0x5b, "Bracket Left");
  static readonly backslash = LogicalKeyboardKey.define(0x5c, "Backslash");
  static readonly bracketRight = LogicalKeyboardKey.define(0x5d, "Bracket Right");
  static readonly backquote = LogicalKeyboardKey.define(0x60, "Backquote");
  static readonly keyA = LogicalKeyboardKey.define(0x61, "Key A");
  static readonly keyB = LogicalKeyboardKey.define(0x62, "Key B");
  static readonly keyC = LogicalKeyboardKey.define(0x63, "Key C");
  static readonly keyD = LogicalKeyboardKey.define(0x64, "Key D");
  static readonly keyE = LogicalKeyboardKey.define(0x65, "Key E");
  static readonly keyF = LogicalKeyboardKey.define(0x66, "Key F");
  static readonly keyG = LogicalKeyboardKey.define(0x67, "Key G");
  static readonly keyH = LogicalKeyboardKey.define(0x68, "Key H");
  static readonly keyI = LogicalKeyboardKey.define(0x69, "Key I");
  static readonly keyJ = LogicalKeyboardKey.define(0x6a, "Key J");
  static readonly keyK = LogicalKeyboardKey.define(0x6b, "Key K");
  static readonly keyL = LogicalKeyboardKey.define(0x6c, "Key L");
  static readonly keyM = LogicalKeyboardKey.define(0x6d, "Key M");
  static readonly keyN = LogicalKeyboardKey.define(0x6e, "Key N");
  static readonly keyO = LogicalKeyboardKey.define(0x6f, "Key O");
  static readonly keyP = LogicalKeyboardKey.define(0x70, "Key P");
  static readonly keyQ = LogicalKeyboardKey.define(0x71, "Key Q");
  static readonly keyR = LogicalKeyboardKey.define(0x72, "Key R");
  static readonly keyS = LogicalKeyboardKey.define(0x73, "Key S");
  static readonly keyT = LogicalKeyboardKey.define(0x74, "Key T");
  static readonly keyU = LogicalKeyboardKey.define(0x75, "Key U");
  static readonly keyV = LogicalKeyboardKey.define(0x76, "Key V");
  static readonly keyW = LogicalKeyboardKey.define(0x77, "Key W");
  static readonly keyX = LogicalKeyboardKey.define(0x78, "Key X");
  static readonly keyY = LogicalKeyboardKey.define(0x79, "Key Y");
  static readonly keyZ = LogicalKeyboardKey.define(0x7a, "Key Z");

  // ---- 特殊键:0x1_0000_0000 + HID usage ----
  static readonly enter = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x28, "Enter");
  static readonly escape = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x29, "Escape");
  static readonly backspace = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x2a, "Backspace");
  static readonly tab = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x2b, "Tab");
  static readonly capsLock = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x39, "Caps Lock");
  static readonly f1 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x3a, "F1");
  static readonly f2 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x3b, "F2");
  static readonly f3 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x3c, "F3");
  static readonly f4 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x3d, "F4");
  static readonly f5 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x3e, "F5");
  static readonly f6 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x3f, "F6");
  static readonly f7 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x40, "F7");
  static readonly f8 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x41, "F8");
  static readonly f9 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x42, "F9");
  static readonly f10 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x43, "F10");
  static readonly f11 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x44, "F11");
  static readonly f12 = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x45, "F12");
  static readonly insert = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x49, "Insert");
  static readonly home = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x4a, "Home");
  static readonly pageUp = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x4b, "Page Up");
  static readonly delete = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x4c, "Delete");
  static readonly end = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x4d, "End");
  static readonly pageDown = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x4e, "Page Down");
  static readonly arrowRight = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x4f, "Arrow Right");
  static readonly arrowLeft = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x50, "Arrow Left");
  static readonly arrowDown = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x51, "Arrow Down");
  static readonly arrowUp = LogicalKeyboardKey.define(SPECIAL_PLANE + 0x52, "Arrow Up");
  static readonly controlLeft = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe0, "Control Left");
  static readonly shiftLeft = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe1, "Shift Left");
  static readonly altLeft = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe2, "Alt Left");
  static readonly metaLeft = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe3, "Meta Left");
  static readonly controlRight = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe4, "Control Right");
  static readonly shiftRight = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe5, "Shift Right");
  static readonly altRight = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe6, "Alt Right");
  static readonly metaRight = LogicalKeyboardKey.define(SPECIAL_PLANE + 0xe7, "Meta Right");

  // ---- 合成键(左右任一)----
  static readonly shift = LogicalKeyboardKey.define(SYNONYM_PLANE + 0x01, "Shift");
  static readonly control = LogicalKeyboardKey.define(SYNONYM_PLANE + 0x02, "Control");
  static readonly alt = LogicalKeyboardKey.define(SYNONYM_PLANE + 0x03, "Alt");
  static readonly meta = LogicalKeyboardKey.define(SYNONYM_PLANE + 0x04, "Meta");
}

const synonymOf = new Map<number, LogicalKeyboardKey>([
  [LogicalKeyboardKey.shiftLeft.keyId, LogicalKeyboardKey.shift],
  [LogicalKeyboardKey.shiftRight.keyId, LogicalKeyboardKey.shift],
  [LogicalKeyboardKey.controlLeft.keyId, LogicalKeyboardKey.control],
  [LogicalKeyboardKey.controlRight.keyId, LogicalKeyboardKey.control],
  [LogicalKeyboardKey.altLeft.keyId, LogicalKeyboardKey.alt],
  [LogicalKeyboardKey.altRight.keyId, LogicalKeyboardKey.alt],
  [LogicalKeyboardKey.metaLeft.keyId, LogicalKeyboardKey.meta],
  [LogicalKeyboardKey.metaRight.keyId, LogicalKeyboardKey.meta],
]);

/** 物理键(键盘上的位置, USB HID usage)。实例按 usage 驻留。 */
export class PhysicalKeyboardKey {
  readonly keyId: number;
  readonly debugName: string | null;

  private constructor(keyId: number, debugName: string | null) {
    this.keyId = keyId;
    this.debugName = debugName;
  }

  static findKeyByKeyId(keyId: number): PhysicalKeyboardKey {
    let k = physicalRegistry.get(keyId);
    if (k === undefined) {
      k = new PhysicalKeyboardKey(keyId, null);
      physicalRegistry.set(keyId, k);
    }
    return k;
  }

  /** @internal 定义常量。 */
  static define(usage: number, debugName: string): PhysicalKeyboardKey {
    const k = new PhysicalKeyboardKey(HID_PAGE + usage, debugName);
    physicalRegistry.set(k.keyId, k);
    return k;
  }

  /** HID usage page。 */
  get usagePage(): number {
    return Math.floor(this.keyId / 0x10000);
  }

  equals(other: PhysicalKeyboardKey | null | undefined): boolean {
    return other != null && other.keyId === this.keyId;
  }

  toString(): string {
    return `PhysicalKeyboardKey#0x${this.keyId.toString(16)}(${this.debugName ?? "unknown"})`;
  }

  static readonly keyA = PhysicalKeyboardKey.define(0x04, "Key A");
  static readonly keyB = PhysicalKeyboardKey.define(0x05, "Key B");
  static readonly keyC = PhysicalKeyboardKey.define(0x06, "Key C");
  static readonly keyD = PhysicalKeyboardKey.define(0x07, "Key D");
  static readonly keyE = PhysicalKeyboardKey.define(0x08, "Key E");
  static readonly keyF = PhysicalKeyboardKey.define(0x09, "Key F");
  static readonly keyG = PhysicalKeyboardKey.define(0x0a, "Key G");
  static readonly keyH = PhysicalKeyboardKey.define(0x0b, "Key H");
  static readonly keyI = PhysicalKeyboardKey.define(0x0c, "Key I");
  static readonly keyJ = PhysicalKeyboardKey.define(0x0d, "Key J");
  static readonly keyK = PhysicalKeyboardKey.define(0x0e, "Key K");
  static readonly keyL = PhysicalKeyboardKey.define(0x0f, "Key L");
  static readonly keyM = PhysicalKeyboardKey.define(0x10, "Key M");
  static readonly keyN = PhysicalKeyboardKey.define(0x11, "Key N");
  static readonly keyO = PhysicalKeyboardKey.define(0x12, "Key O");
  static readonly keyP = PhysicalKeyboardKey.define(0x13, "Key P");
  static readonly keyQ = PhysicalKeyboardKey.define(0x14, "Key Q");
  static readonly keyR = PhysicalKeyboardKey.define(0x15, "Key R");
  static readonly keyS = PhysicalKeyboardKey.define(0x16, "Key S");
  static readonly keyT = PhysicalKeyboardKey.define(0x17, "Key T");
  static readonly keyU = PhysicalKeyboardKey.define(0x18, "Key U");
  static readonly keyV = PhysicalKeyboardKey.define(0x19, "Key V");
  static readonly keyW = PhysicalKeyboardKey.define(0x1a, "Key W");
  static readonly keyX = PhysicalKeyboardKey.define(0x1b, "Key X");
  static readonly keyY = PhysicalKeyboardKey.define(0x1c, "Key Y");
  static readonly keyZ = PhysicalKeyboardKey.define(0x1d, "Key Z");
  static readonly digit1 = PhysicalKeyboardKey.define(0x1e, "Digit 1");
  static readonly digit2 = PhysicalKeyboardKey.define(0x1f, "Digit 2");
  static readonly digit3 = PhysicalKeyboardKey.define(0x20, "Digit 3");
  static readonly digit4 = PhysicalKeyboardKey.define(0x21, "Digit 4");
  static readonly digit5 = PhysicalKeyboardKey.define(0x22, "Digit 5");
  static readonly digit6 = PhysicalKeyboardKey.define(0x23, "Digit 6");
  static readonly digit7 = PhysicalKeyboardKey.define(0x24, "Digit 7");
  static readonly digit8 = PhysicalKeyboardKey.define(0x25, "Digit 8");
  static readonly digit9 = PhysicalKeyboardKey.define(0x26, "Digit 9");
  static readonly digit0 = PhysicalKeyboardKey.define(0x27, "Digit 0");
  static readonly enter = PhysicalKeyboardKey.define(0x28, "Enter");
  static readonly escape = PhysicalKeyboardKey.define(0x29, "Escape");
  static readonly backspace = PhysicalKeyboardKey.define(0x2a, "Backspace");
  static readonly tab = PhysicalKeyboardKey.define(0x2b, "Tab");
  static readonly space = PhysicalKeyboardKey.define(0x2c, "Space");
  static readonly minus = PhysicalKeyboardKey.define(0x2d, "Minus");
  static readonly equal = PhysicalKeyboardKey.define(0x2e, "Equal");
  static readonly bracketLeft = PhysicalKeyboardKey.define(0x2f, "Bracket Left");
  static readonly bracketRight = PhysicalKeyboardKey.define(0x30, "Bracket Right");
  static readonly backslash = PhysicalKeyboardKey.define(0x31, "Backslash");
  static readonly semicolon = PhysicalKeyboardKey.define(0x33, "Semicolon");
  static readonly quote = PhysicalKeyboardKey.define(0x34, "Quote");
  static readonly backquote = PhysicalKeyboardKey.define(0x35, "Backquote");
  static readonly comma = PhysicalKeyboardKey.define(0x36, "Comma");
  static readonly period = PhysicalKeyboardKey.define(0x37, "Period");
  static readonly slash = PhysicalKeyboardKey.define(0x38, "Slash");
  static readonly capsLock = PhysicalKeyboardKey.define(0x39, "Caps Lock");
  static readonly f1 = PhysicalKeyboardKey.define(0x3a, "F1");
  static readonly f2 = PhysicalKeyboardKey.define(0x3b, "F2");
  static readonly f3 = PhysicalKeyboardKey.define(0x3c, "F3");
  static readonly f4 = PhysicalKeyboardKey.define(0x3d, "F4");
  static readonly f5 = PhysicalKeyboardKey.define(0x3e, "F5");
  static readonly f6 = PhysicalKeyboardKey.define(0x3f, "F6");
  static readonly f7 = PhysicalKeyboardKey.define(0x40, "F7");
  static readonly f8 = PhysicalKeyboardKey.define(0x41, "F8");
  static readonly f9 = PhysicalKeyboardKey.define(0x42, "F9");
  static readonly f10 = PhysicalKeyboardKey.define(0x43, "F10");
  static readonly f11 = PhysicalKeyboardKey.define(0x44, "F11");
  static readonly f12 = PhysicalKeyboardKey.define(0x45, "F12");
  static readonly insert = PhysicalKeyboardKey.define(0x49, "Insert");
  static readonly home = PhysicalKeyboardKey.define(0x4a, "Home");
  static readonly pageUp = PhysicalKeyboardKey.define(0x4b, "Page Up");
  static readonly delete = PhysicalKeyboardKey.define(0x4c, "Delete");
  static readonly end = PhysicalKeyboardKey.define(0x4d, "End");
  static readonly pageDown = PhysicalKeyboardKey.define(0x4e, "Page Down");
  static readonly arrowRight = PhysicalKeyboardKey.define(0x4f, "Arrow Right");
  static readonly arrowLeft = PhysicalKeyboardKey.define(0x50, "Arrow Left");
  static readonly arrowDown = PhysicalKeyboardKey.define(0x51, "Arrow Down");
  static readonly arrowUp = PhysicalKeyboardKey.define(0x52, "Arrow Up");
  static readonly controlLeft = PhysicalKeyboardKey.define(0xe0, "Control Left");
  static readonly shiftLeft = PhysicalKeyboardKey.define(0xe1, "Shift Left");
  static readonly altLeft = PhysicalKeyboardKey.define(0xe2, "Alt Left");
  static readonly metaLeft = PhysicalKeyboardKey.define(0xe3, "Meta Left");
  static readonly controlRight = PhysicalKeyboardKey.define(0xe4, "Control Right");
  static readonly shiftRight = PhysicalKeyboardKey.define(0xe5, "Shift Right");
  static readonly altRight = PhysicalKeyboardKey.define(0xe6, "Alt Right");
  static readonly metaRight = PhysicalKeyboardKey.define(0xe7, "Meta Right");
}
