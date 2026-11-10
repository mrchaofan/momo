import { ValueNotifier } from "../foundation/change_notifier";
import { reportError } from "../foundation/assert";
import { isApplePlatform } from "../foundation/platform";
import type { Key } from "../foundation/key";
import { PanGestureRecognizer, type DragStartDetails, type DragUpdateDetails } from "../gestures/drag";
import { DoubleTapGestureRecognizer } from "../gestures/multitap";
import type { GestureRecognizer } from "../gestures/recognizer";
import { kPrimaryButton, type PointerDownEvent } from "../gestures/events";
import { Color } from "../painting/color";
import { Offset, type TextDirection } from "../painting/geometry";
import { TextSpan } from "../painting/text_span";
import { TextStyle } from "../painting/text_style";
import { RenderEditable } from "../rendering/editable";
import { SchedulerBinding, type DelayedTask } from "../scheduler/binding";
import { Clipboard } from "../services/clipboard";
import { HardwareKeyboard, KeyDownEvent, KeyRepeatEvent, type KeyEvent, type KeyEventResult } from "../services/hardware_keyboard";
import { LogicalKeyboardKey } from "../services/keyboard_key";
import { TextEditingValue, TextRange, TextSelection } from "../services/text_editing";
import { TextInput, type TextInputClient, type TextInputConnection } from "../services/text_input";
import { Listener, MouseRegion } from "./basic";
import { Focus } from "./focus_scope";
import { FocusNode } from "./focus_manager";
import {
  GlobalKey, LeafRenderObjectWidget, State, StatefulWidget, type BuildContext, type Type, type Widget,
} from "./framework";
import { RawGestureDetector, gestureFactory, type GestureRecognizerFactory } from "./gesture_detector";
import { MediaQuery } from "./media_query";
import { DefaultTextStyle } from "./text";

/** 文本编辑控制器:持有 TextEditingValue(文本/选区/composing)。 */
export class TextEditingController extends ValueNotifier<TextEditingValue> {
  constructor(o: { text?: string } | TextEditingValue = {}) {
    super(o instanceof TextEditingValue ? o : new TextEditingValue({ text: o.text ?? "", selection: TextSelection.collapsed(-1) }));
  }
  get text(): string {
    return this.value.text;
  }
  /** 设置文本:光标移到末尾, 清除 composing。 */
  set text(v: string) {
    this.value = new TextEditingValue({ text: v, selection: TextSelection.collapsed(v.length), composing: TextRange.empty });
  }
  get selection(): TextSelection {
    return this.value.selection;
  }
  set selection(s: TextSelection) {
    this.value = this.value.copyWith({ selection: s });
  }
  clear(): void {
    this.value = new TextEditingValue({ text: "", selection: TextSelection.collapsed(0) });
  }
  /** 构建显示用 TextSpan(composing 区间加下划线)。 */
  buildTextSpan(style: TextStyle, withComposing: boolean, obscure: boolean): TextSpan {
    const v = this.value;
    const text = obscure ? "•".repeat(v.text.length) : v.text;
    if (withComposing && v.isComposingRangeValid && !v.composing.isCollapsed) {
      const c = v.composing;
      return new TextSpan({
        style,
        children: [
          new TextSpan({ text: text.substring(0, c.start) }),
          new TextSpan({ text: text.substring(c.start, c.end), style: new TextStyle({ decoration: "underline" }) }),
          new TextSpan({ text: text.substring(c.end) }),
        ],
      });
    }
    return new TextSpan({ style, text });
  }
}

/** EditableText 参数。 */
export interface EditableTextOptions {
  key?: Key | null;
  controller: TextEditingController;
  focusNode: FocusNode;
  style?: TextStyle | null;
  cursorColor?: Color;
  selectionColor?: Color;
  cursorWidth?: number;
  /** 1 = 单行(默认);null = 不限行数。 */
  maxLines?: number | null;
  minLines?: number | null;
  obscureText?: boolean;
  readOnly?: boolean;
  autofocus?: boolean;
  /** 光标闪烁(基于帧回调计时, 聚焦期间持续出帧)。 */
  cursorBlink?: boolean;
  textAlign?: "left" | "right" | "center" | "start" | "end";
  textDirection?: TextDirection;
  onChanged?: ((text: string) => void) | null;
  onSubmitted?: ((text: string) => void) | null;
  onSelectionChanged?: ((selection: TextSelection) => void) | null;
}

/** 光标闪烁半周期(毫秒)。 */
export const kCursorBlinkHalfPeriod = 500;

/**
 * 基础可编辑文本(对标 Flutter EditableText):经 momo/textinput 与平台输入法交互
 * (show/hide/setEditingState/setCaretRect;处理 commit/composing/composingEnd/command),
 * 键盘导航与快捷键、鼠标点击/拖选/双击选词、光标与选区绘制。
 */
export class EditableText extends StatefulWidget {
  readonly o: EditableTextOptions;
  constructor(o: EditableTextOptions) {
    super(o.key);
    this.o = o;
  }
  get controller(): TextEditingController {
    return this.o.controller;
  }
  get focusNode(): FocusNode {
    return this.o.focusNode;
  }
  createState(): State {
    return new EditableTextState();
  }
}

class Editable extends LeafRenderObjectWidget {
  constructor(
    key: Key,
    readonly text: TextSpan,
    readonly selection: TextSelection,
    readonly showCursor: ValueNotifier<boolean>,
    readonly hasFocus: boolean,
    readonly cursorColor: Color,
    readonly selectionColor: Color,
    readonly cursorWidth: number,
    readonly maxLines: number | null,
    readonly minLines: number | null,
    readonly textAlign: "left" | "right" | "center" | "start" | "end",
    readonly textScaleFactor: number,
  ) {
    super(key);
  }
  createRenderObject(): RenderEditable {
    return new RenderEditable({
      text: this.text, selection: this.selection, showCursor: this.showCursor, hasFocus: this.hasFocus,
      cursorColor: this.cursorColor, selectionColor: this.selectionColor, cursorWidth: this.cursorWidth,
      maxLines: this.maxLines, minLines: this.minLines, textAlign: this.textAlign, textScaleFactor: this.textScaleFactor,
    });
  }
  override updateRenderObject(_c: BuildContext, r: RenderEditable): void {
    r.text = this.text;
    r.selection = this.selection;
    r.showCursor = this.showCursor;
    r.hasFocus = this.hasFocus;
    r.cursorColor = this.cursorColor;
    r.selectionColor = this.selectionColor;
    r.cursorWidth = this.cursorWidth;
    r.maxLines = this.maxLines;
    r.minLines = this.minLines;
    r.textAlign = this.textAlign;
    r.textScaleFactor = this.textScaleFactor;
  }
}

function isLowSurrogate(c: number): boolean {
  return c >= 0xdc00 && c <= 0xdfff;
}
function isHighSurrogate(c: number): boolean {
  return c >= 0xd800 && c <= 0xdbff;
}
function prevOffset(text: string, o: number): number {
  if (o <= 0) return 0;
  return o >= 2 && isLowSurrogate(text.charCodeAt(o - 1)) && isHighSurrogate(text.charCodeAt(o - 2)) ? o - 2 : o - 1;
}
function nextOffset(text: string, o: number): number {
  if (o >= text.length) return text.length;
  return isHighSurrogate(text.charCodeAt(o)) && o + 1 < text.length && isLowSurrogate(text.charCodeAt(o + 1)) ? o + 2 : o + 1;
}
const isSpace = (c: string | undefined): boolean => c === " " || c === "\t" || c === "\n";
function wordLeft(text: string, o: number): number {
  let i = o;
  while (i > 0 && isSpace(text[i - 1])) i--;
  while (i > 0 && !isSpace(text[i - 1])) i--;
  return i;
}
function wordRight(text: string, o: number): number {
  let i = o;
  while (i < text.length && isSpace(text[i])) i++;
  while (i < text.length && !isSpace(text[i])) i++;
  return i;
}

export class EditableTextState extends State<EditableText> implements TextInputClient {
  #connection: TextInputConnection | null = null;
  readonly #cursorVisible = new ValueNotifier(false);
  #blink: DelayedTask | null = null;
  #editableKey = new GlobalKey();
  #lastCaretRect = "";
  #caretUpdateScheduled = false;
  #dragBase = -1;

  get #controller(): TextEditingController {
    return this.widget.controller;
  }
  get #value(): TextEditingValue {
    return this.widget.controller.value;
  }
  get #multiline(): boolean {
    return this.widget.o.maxLines !== 1;
  }
  /** 当前输入法连接(测试用)。 */
  get textInputConnection(): TextInputConnection | null {
    return this.#connection;
  }
  /** 光标当前是否显示(闪烁状态)。 */
  get cursorCurrentlyVisible(): boolean {
    return this.#cursorVisible.value;
  }
  /** 渲染对象。 */
  get renderEditable(): RenderEditable | null {
    const ro = this.#editableKey.currentContext?.findRenderObject() ?? null;
    return ro instanceof RenderEditable ? ro : null;
  }

  override initState(): void {
    this.#controller.addListener(this.#didChangeValue);
    this.widget.focusNode.addListener(this.#handleFocusChanged);
  }

  override didUpdateWidget(old: EditableText): void {
    if (old.controller !== this.widget.controller) {
      old.controller.removeListener(this.#didChangeValue);
      this.widget.controller.addListener(this.#didChangeValue);
      this.#syncToPlatform();
    }
    if (old.focusNode !== this.widget.focusNode) {
      old.focusNode.removeListener(this.#handleFocusChanged);
      this.widget.focusNode.addListener(this.#handleFocusChanged);
    }
  }

  override dispose(): void {
    this.#controller.removeListener(this.#didChangeValue);
    this.widget.focusNode.removeListener(this.#handleFocusChanged);
    this.#closeConnection();
    this.#stopBlink();
    this.#cursorVisible.dispose();
  }

  // ---------------------------------------------------------------------------
  // 焦点与输入法连接

  #handleFocusChanged = (): void => {
    if (this.widget.focusNode.hasFocus) {
      const v = this.#value;
      if (!v.selection.isValid) {
        this.#controller.value = v.copyWith({ selection: TextSelection.collapsed(v.text.length) });
      }
      this.#openConnection();
      this.#startBlink();
    } else {
      this.#closeConnection();
      this.#stopBlink();
      const v = this.#value;
      if (v.isComposingRangeValid) this.#controller.value = v.copyWith({ composing: TextRange.empty });
    }
    if (this.mounted) this.setState();
  };

  #openConnection(): void {
    if (this.widget.o.readOnly) return;
    if (this.#connection !== null && this.#connection.attached) return;
    this.#connection = TextInput.attach(this);
    this.#connection.show();
    this.#connection.setEditingState(this.#value);
    this.#lastCaretRect = "";
    this.#scheduleCaretRectUpdate();
  }

  #closeConnection(): void {
    if (this.#connection !== null) {
      this.#connection.close();
      this.#connection = null;
    }
  }

  connectionClosed(): void {
    this.#connection = null;
  }

  #syncToPlatform(): void {
    if (this.#connection !== null && this.#connection.attached) this.#connection.setEditingState(this.#value);
  }

  #didChangeValue = (): void => {
    this.#syncToPlatform();
    this.#resetBlink();
    this.widget.o.onSelectionChanged?.(this.#value.selection);
    this.#scheduleCaretRectUpdate();
    if (this.mounted) this.setState();
  };

  #scheduleCaretRectUpdate(): void {
    if (this.#caretUpdateScheduled) return;
    this.#caretUpdateScheduled = true;
    SchedulerBinding.instance.addPostFrameCallback(() => {
      this.#caretUpdateScheduled = false;
      this.#updateCaretRect();
    });
  }

  #updateCaretRect(): void {
    const conn = this.#connection;
    const ro = this.renderEditable;
    if (conn === null || !conn.attached || ro === null || !ro.attached || !ro.hasSize) return;
    const sel = this.#value.selection;
    if (!sel.isValid) return;
    const local = ro.getLocalRectForCaret(sel.extentOffset, sel.affinity);
    const tl = ro.localToGlobal(local.topLeft);
    const dpr = MediaQuery.maybeOf(this.context)?.devicePixelRatio ?? 1;
    const x = tl.dx * dpr, y = tl.dy * dpr, w = local.width * dpr, h = local.height * dpr;
    const key = `${x},${y},${w},${h}`;
    if (key === this.#lastCaretRect) return;
    this.#lastCaretRect = key;
    conn.setCaretRect(x, y, w, h);
  }

  // ---------------------------------------------------------------------------
  // 光标闪烁

  #startBlink(): void {
    this.#cursorVisible.value = true;
    this.#stopBlinkTimer();
    if (this.widget.o.cursorBlink === false) return;
    const tick = (): void => {
      this.#cursorVisible.value = !this.#cursorVisible.value;
      this.#blink = SchedulerBinding.instance.scheduleDelayed(kCursorBlinkHalfPeriod, tick);
    };
    this.#blink = SchedulerBinding.instance.scheduleDelayed(kCursorBlinkHalfPeriod, tick);
  }
  #stopBlinkTimer(): void {
    this.#blink?.cancel();
    this.#blink = null;
  }
  #stopBlink(): void {
    this.#stopBlinkTimer();
    this.#cursorVisible.value = false;
  }
  #resetBlink(): void {
    if (this.widget.focusNode.hasFocus) this.#startBlink();
  }

  // ---------------------------------------------------------------------------
  // 编辑操作

  /** 以用户编辑方式更新值(触发 onChanged)。 */
  userUpdateValue(next: TextEditingValue): void {
    const old = this.#value;
    if (this.widget.o.readOnly && next.text !== old.text) return;
    this.#controller.value = next;
    if (next.text !== old.text) {
      try {
        this.widget.o.onChanged?.(next.text);
      } catch (e) {
        reportError(e, "in EditableText.onChanged");
      }
    }
  }

  #selectionRange(v: TextEditingValue): TextRange {
    const s = v.selection;
    if (!s.isValid) return TextRange.collapsed(v.text.length);
    return new TextRange(Math.min(s.start, v.text.length), Math.min(s.end, v.text.length));
  }

  #clampRange(r: TextRange, len: number): TextRange {
    const s = Math.max(0, Math.min(r.start, len));
    const e = Math.max(s, Math.min(r.end, len));
    return new TextRange(s, e);
  }

  /** 把 range 替换为 text, 光标置于其后。 */
  #replace(range: TextRange, text: string): void {
    const v = this.#value;
    const r = this.#clampRange(range, v.text.length);
    const newText = v.text.substring(0, r.start) + text + v.text.substring(r.end);
    this.userUpdateValue(new TextEditingValue({ text: newText, selection: TextSelection.collapsed(r.start + text.length), composing: TextRange.empty }));
  }

  commitText(text: string, replace?: TextRange | null): void {
    const v = this.#value;
    let t = text;
    if (!this.#multiline) t = t.replace(/[\r\n]/g, "");
    const range = replace ?? (v.isComposingRangeValid ? v.composing : this.#selectionRange(v));
    this.#replace(range, t);
  }

  updateComposing(text: string, cursor: number, replace?: TextRange | null): void {
    const v = this.#value;
    const range = this.#clampRange(replace ?? (v.isComposingRangeValid ? v.composing : this.#selectionRange(v)), v.text.length);
    const newText = v.text.substring(0, range.start) + text + v.text.substring(range.end);
    const c = Math.max(0, Math.min(cursor, text.length));
    this.userUpdateValue(new TextEditingValue({
      text: newText,
      selection: TextSelection.collapsed(range.start + c),
      composing: text.length > 0 ? new TextRange(range.start, range.start + text.length) : TextRange.empty,
    }));
  }

  endComposing(): void {
    const v = this.#value;
    if (!v.isComposingRangeValid) return;
    // 输入法取消预编辑:移除未提交的预编辑文本。
    const c = v.composing;
    const newText = v.text.substring(0, c.start) + v.text.substring(c.end);
    this.userUpdateValue(new TextEditingValue({ text: newText, selection: TextSelection.collapsed(c.start), composing: TextRange.empty }));
  }

  #moveTo(offset: number, extend: boolean): void {
    const v = this.#value;
    const o = Math.max(0, Math.min(offset, v.text.length));
    const sel = extend && v.selection.isValid ? v.selection.extendTo(o) : TextSelection.collapsed(o);
    this.#controller.value = v.copyWith({ selection: sel, composing: TextRange.empty });
  }

  #lineBounds(offset: number): { start: number; end: number } {
    const text = this.#value.text;
    const ro = this.renderEditable;
    if (ro !== null && ro.hasSize) {
      const metrics = ro.computeLineMetrics();
      for (const m of metrics) {
        if (offset >= m.startIndex && (offset < m.endIndex || (offset === m.endIndex && (m.hardBreak || m === metrics[metrics.length - 1])))) {
          let end = m.endIndex;
          if (end > m.startIndex && text[end - 1] === "\n") end--;
          return { start: m.startIndex, end };
        }
      }
    }
    let s = offset;
    while (s > 0 && text[s - 1] !== "\n") s--;
    let e = offset;
    while (e < text.length && text[e] !== "\n") e++;
    return { start: s, end: e };
  }

  #verticalMove(dir: number, extend: boolean): void {
    const v = this.#value;
    const ro = this.renderEditable;
    const sel = v.selection;
    if (ro === null || !sel.isValid) return;
    const caret = ro.getLocalRectForCaret(sel.extentOffset, sel.affinity);
    const lh = ro.preferredLineHeight;
    const y = caret.top + lh / 2 + dir * lh;
    if (y < 0) return this.#moveTo(0, extend);
    if (y > ro.textPainter.height + ro.scrollOffset) return this.#moveTo(v.text.length, extend);
    const pos = ro.getPositionForPoint(new Offset(caret.left, y));
    this.#moveTo(pos.offset, extend);
  }

  #delete(forward: boolean, word: boolean, toLineEdge = false): void {
    const v = this.#value;
    const sel = this.#selectionRange(v);
    if (!sel.isCollapsed) return this.#replace(sel, "");
    const o = sel.start;
    let target: number;
    if (toLineEdge) target = forward ? this.#lineBounds(o).end : this.#lineBounds(o).start;
    else if (word) target = forward ? wordRight(v.text, o) : wordLeft(v.text, o);
    else target = forward ? nextOffset(v.text, o) : prevOffset(v.text, o);
    if (target === o) return;
    this.#replace(new TextRange(Math.min(o, target), Math.max(o, target)), "");
  }

  #submit(): void {
    try {
      this.widget.o.onSubmitted?.(this.#value.text);
    } catch (e) {
      reportError(e, "in EditableText.onSubmitted");
    }
  }

  #selectedText(): string {
    const v = this.#value;
    const s = this.#selectionRange(v);
    return v.text.substring(s.start, s.end);
  }

  /** 复制选区到剪贴板。 */
  copySelection(): void {
    const t = this.#selectedText();
    if (t.length > 0 && !this.widget.o.obscureText) void Clipboard.setData(t).catch((e: unknown) => reportError(e, "copying text"));
  }
  /** 剪切。 */
  cutSelection(): void {
    if (this.widget.o.readOnly) return;
    this.copySelection();
    this.#replace(this.#selectionRange(this.#value), "");
  }
  /** 粘贴(异步)。 */
  pasteText(): Promise<void> {
    if (this.widget.o.readOnly) return Promise.resolve();
    return Clipboard.getData().then((t) => {
      if (t !== null && this.mounted) this.commitText(t, this.#selectionRange(this.#value));
    }, (e: unknown) => reportError(e, "pasting text"));
  }
  /** 全选。 */
  selectAll(): void {
    const v = this.#value;
    this.#controller.value = v.copyWith({ selection: new TextSelection(0, v.text.length) });
  }

  performCommand(command: string): void {
    const v = this.#value;
    const ext = v.selection.isValid ? v.selection.extentOffset : v.text.length;
    switch (command) {
      case "deleteBackward": return this.#delete(false, false);
      case "deleteForward": return this.#delete(true, false);
      case "deleteWordBackward": return this.#delete(false, true);
      case "deleteWordForward": return this.#delete(true, true);
      case "deleteToBeginningOfLine": return this.#delete(false, false, true);
      case "deleteToEndOfLine": return this.#delete(true, false, true);
      case "insertNewline":
        if (this.#multiline) this.#replace(this.#selectionRange(v), "\n");
        else this.#submit();
        return;
      case "insertTab":
        this.widget.focusNode.nextFocus();
        return;
      case "insertBacktab":
        this.widget.focusNode.previousFocus();
        return;
      case "moveLeft": return this.#moveTo(v.selection.isCollapsed ? prevOffset(v.text, ext) : v.selection.start, false);
      case "moveRight": return this.#moveTo(v.selection.isCollapsed ? nextOffset(v.text, ext) : v.selection.end, false);
      case "moveLeftAndModifySelection": return this.#moveTo(prevOffset(v.text, ext), true);
      case "moveRightAndModifySelection": return this.#moveTo(nextOffset(v.text, ext), true);
      case "moveWordLeft": return this.#moveTo(wordLeft(v.text, ext), false);
      case "moveWordRight": return this.#moveTo(wordRight(v.text, ext), false);
      case "moveWordLeftAndModifySelection": return this.#moveTo(wordLeft(v.text, ext), true);
      case "moveWordRightAndModifySelection": return this.#moveTo(wordRight(v.text, ext), true);
      case "moveUp": return this.#verticalMove(-1, false);
      case "moveDown": return this.#verticalMove(1, false);
      case "moveUpAndModifySelection": return this.#verticalMove(-1, true);
      case "moveDownAndModifySelection": return this.#verticalMove(1, true);
      case "moveToLineStart":
      case "moveToBeginningOfLine":
      case "moveToLeftEndOfLine": return this.#moveTo(this.#lineBounds(ext).start, false);
      case "moveToLineEnd":
      case "moveToEndOfLine":
      case "moveToRightEndOfLine": return this.#moveTo(this.#lineBounds(ext).end, false);
      case "moveToBeginningOfLineAndModifySelection":
      case "moveToLeftEndOfLineAndModifySelection": return this.#moveTo(this.#lineBounds(ext).start, true);
      case "moveToEndOfLineAndModifySelection":
      case "moveToRightEndOfLineAndModifySelection": return this.#moveTo(this.#lineBounds(ext).end, true);
      case "moveToBeginningOfDocument": return this.#moveTo(0, false);
      case "moveToEndOfDocument": return this.#moveTo(v.text.length, false);
      case "moveToBeginningOfDocumentAndModifySelection": return this.#moveTo(0, true);
      case "moveToEndOfDocumentAndModifySelection": return this.#moveTo(v.text.length, true);
      case "selectAll": return this.selectAll();
      case "copy": return this.copySelection();
      case "cut": return this.cutSelection();
      case "paste": void this.pasteText(); return;
      case "cancelOperation":
        this.widget.focusNode.unfocus();
        return;
      default:
        return;
    }
  }

  // ---------------------------------------------------------------------------
  // 键盘

  #onKey = (_node: FocusNode, event: KeyEvent): KeyEventResult => {
    if (!(event instanceof KeyDownEvent) && !(event instanceof KeyRepeatEvent)) return "ignored";
    // 预编辑中, 按键交给输入法。
    if (this.#value.isComposingRangeValid && !this.#value.composing.isCollapsed) return "skipRemainingHandlers";
    const kb = HardwareKeyboard.instance;
    const apple = isApplePlatform();
    const primary = apple ? kb.isMetaPressed : kb.isControlPressed;
    const wordMod = apple ? kb.isAltPressed : kb.isControlPressed;
    const lineMod = apple && kb.isMetaPressed;
    const shift = kb.isShiftPressed;
    const k = event.logicalKey;
    const L = LogicalKeyboardKey;
    const v = this.#value;
    const ext = v.selection.isValid ? v.selection.extentOffset : v.text.length;
    if (k === L.arrowLeft) {
      if (lineMod) this.#moveTo(this.#lineBounds(ext).start, shift);
      else if (wordMod) this.#moveTo(wordLeft(v.text, ext), shift);
      else if (!shift && !v.selection.isCollapsed) this.#moveTo(v.selection.start, false);
      else this.#moveTo(prevOffset(v.text, ext), shift);
      return "handled";
    }
    if (k === L.arrowRight) {
      if (lineMod) this.#moveTo(this.#lineBounds(ext).end, shift);
      else if (wordMod) this.#moveTo(wordRight(v.text, ext), shift);
      else if (!shift && !v.selection.isCollapsed) this.#moveTo(v.selection.end, false);
      else this.#moveTo(nextOffset(v.text, ext), shift);
      return "handled";
    }
    if (k === L.arrowUp) {
      if (!this.#multiline || lineMod) this.#moveTo(0, shift);
      else this.#verticalMove(-1, shift);
      return "handled";
    }
    if (k === L.arrowDown) {
      if (!this.#multiline || lineMod) this.#moveTo(v.text.length, shift);
      else this.#verticalMove(1, shift);
      return "handled";
    }
    if (k === L.home) {
      this.#moveTo(kb.isControlPressed ? 0 : this.#lineBounds(ext).start, shift);
      return "handled";
    }
    if (k === L.end) {
      this.#moveTo(kb.isControlPressed ? v.text.length : this.#lineBounds(ext).end, shift);
      return "handled";
    }
    if (k === L.backspace) {
      if (lineMod) this.#delete(false, false, true);
      else this.#delete(false, wordMod);
      return "handled";
    }
    if (k === L.delete) {
      this.#delete(true, wordMod);
      return "handled";
    }
    if (k === L.enter) {
      this.performCommand("insertNewline");
      return "handled";
    }
    if (primary && !kb.isAltPressed) {
      if (k === L.keyA) { this.selectAll(); return "handled"; }
      if (k === L.keyC) { this.copySelection(); return "handled"; }
      if (k === L.keyX) { this.cutSelection(); return "handled"; }
      if (k === L.keyV) { void this.pasteText(); return "handled"; }
    }
    if (k === L.tab || k === L.escape) return "ignored";
    // 可打印字符:文本经输入法 commit 到达;阻止上层快捷键(如空格激活)。
    if (event.character !== null && event.character.length > 0 && !primary) return "skipRemainingHandlers";
    return "ignored";
  };

  // ---------------------------------------------------------------------------
  // 指针

  #positionAt(globalPosition: Offset): number | null {
    const ro = this.renderEditable;
    if (ro === null || !ro.hasSize) return null;
    return ro.getPositionForPoint(ro.globalToLocal(globalPosition)).offset;
  }

  #onPointerDown = (e: PointerDownEvent): void => {
    if (e.buttons !== kPrimaryButton) return;
    const node = this.widget.focusNode;
    if (!node.hasFocus) node.requestFocus();
    const o = this.#positionAt(e.position);
    if (o !== null) this.#moveTo(o, HardwareKeyboard.instance.isShiftPressed);
  };

  #onDoubleTap = (): void => {
    const v = this.#value;
    const ro = this.renderEditable;
    if (ro === null || !v.selection.isValid) return;
    const b = ro.getWordBoundary(Math.min(v.selection.extentOffset, Math.max(0, v.text.length - 1)));
    this.#controller.value = v.copyWith({ selection: new TextSelection(b.start, b.end) });
  };

  #onPanStart = (d: DragStartDetails): void => {
    const node = this.widget.focusNode;
    if (!node.hasFocus) node.requestFocus();
    const v = this.#value;
    const o = this.#positionAt(d.globalPosition);
    this.#dragBase = HardwareKeyboard.instance.isShiftPressed && v.selection.isValid ? v.selection.baseOffset : (o ?? 0);
    if (o !== null) this.#controller.value = v.copyWith({ selection: new TextSelection(this.#dragBase, o) });
  };

  #onPanUpdate = (d: DragUpdateDetails): void => {
    const o = this.#positionAt(d.globalPosition);
    if (o === null || this.#dragBase < 0) return;
    this.#controller.value = this.#value.copyWith({ selection: new TextSelection(this.#dragBase, o) });
  };

  build(context: BuildContext): Widget {
    const o = this.widget.o;
    const focused = this.widget.focusNode.hasFocus;
    const style = DefaultTextStyle.of(context).style.merge(o.style ?? null);
    const scale = MediaQuery.maybeOf(context)?.textScaleFactor ?? 1;
    const gestures = new Map<Type<GestureRecognizer>, GestureRecognizerFactory<GestureRecognizer>>([
      [DoubleTapGestureRecognizer, gestureFactory(() => new DoubleTapGestureRecognizer(), (r: DoubleTapGestureRecognizer) => {
        r.onDoubleTap = this.#onDoubleTap;
      })],
      [PanGestureRecognizer, gestureFactory(() => new PanGestureRecognizer(), (r: PanGestureRecognizer) => {
        r.onStart = this.#onPanStart;
        r.onUpdate = this.#onPanUpdate;
        r.dragStartBehavior = "down";
      })],
    ]);
    return new MouseRegion({
      cursor: "text",
      child: new Listener({
        onPointerDown: this.#onPointerDown,
        behavior: "opaque",
        child: new RawGestureDetector({
        gestures,
        behavior: "opaque",
        child: new Focus({
          focusNode: this.widget.focusNode,
          autofocus: o.autofocus ?? false,
          onKeyEvent: this.#onKey,
          child: new Editable(
            this.#editableKey,
            this.#controller.buildTextSpan(style, focused, o.obscureText ?? false),
            this.#value.selection,
            this.#cursorVisible,
            focused,
            o.cursorColor ?? new Color(0xff1a73e8),
            o.selectionColor ?? new Color(0x661a73e8),
            o.cursorWidth ?? 2,
            o.maxLines === undefined ? 1 : o.maxLines,
            o.minLines ?? null,
            o.textAlign ?? "start",
            scale,
          ),
        }),
      }),
      }),
    });
  }
}
