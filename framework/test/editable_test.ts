import {
  Center, EditableText, FocusNode, MethodCall, SizedBox, TextEditingController, TextEditingValue, TextRange, TextSelection,
  TextStyle, LogicalKeyboardKey, PhysicalKeyboardKey, KeyModifier, Offset, Align, Alignment, debugOverrideTargetPlatform,
  type Widget, Column,
} from "../packages/momo/src/index";
import { expect, group } from "./harness";
import { Kind } from "./tester";
import { testWidgets } from "./util";
import type { WidgetTester } from "./tester";

function field(controller: TextEditingController, node: FocusNode, o: { maxLines?: number | null; onSubmitted?: (t: string) => void; onChanged?: (t: string) => void } = {}): Widget {
  return new Align({
    alignment: Alignment.topLeft,
    child: new SizedBox({
      width: 300,
      child: new EditableText({
        controller, focusNode: node, cursorBlink: false, style: new TextStyle({ fontSize: 10 }),
        maxLines: o.maxLines === undefined ? 1 : o.maxLines,
        onSubmitted: o.onSubmitted ?? null, onChanged: o.onChanged ?? null,
      }),
    }),
  });
}

async function focused(t: WidgetTester, text = "", maxLines: number | null = 1): Promise<{ c: TextEditingController; n: FocusNode; calls: MethodCall[] }> {
  const calls = t.recordMethodCalls("momo/textinput");
  const c = new TextEditingController({ text });
  const n = new FocusNode();
  await t.pumpWidget(field(c, n, { maxLines }));
  n.requestFocus();
  await t.pump();
  return { c, n, calls };
}

const key = (k: LogicalKeyboardKey, p: PhysicalKeyboardKey): { logical: number; physical: number } => ({ logical: k.keyId, physical: p.keyId });

group("EditableText", () => {
  testWidgets("focus opens the input connection and syncs editing state", async (t) => {
    const { c, calls } = await focused(t, "abc");
    await t.flushMicrotasks();
    const methods = calls.map((m) => m.method);
    expect.deepEqual(methods.slice(0, 2), ["show", "setEditingState"]);
    expect.deepEqual(calls[1]!.args, { text: "abc", selectionBase: 3, selectionExtent: 3, composingStart: -1, composingEnd: -1 });
    expect.isTrue(methods.includes("setCaretRect"), "caret rect reported after layout");
    const caret = calls.find((m) => m.method === "setCaretRect")!.args as number[];
    expect.deepEqual(caret, [30, 0, 2, 10]);
    expect.equal(c.selection.baseOffset, 3);
  }, { ahemText: true });

  testWidgets("commit inserts at the selection; composing / composingEnd", async (t) => {
    const { c, calls } = await focused(t, "hello");
    c.selection = TextSelection.collapsed(2);
    await t.platformCall("momo/textinput", "commit", { text: "XY" });
    expect.equal(c.text, "heXYllo");
    expect.equal(c.selection.baseOffset, 4);
    // 预编辑:插入 composing 区间, 光标在预编辑内
    await t.platformCall("momo/textinput", "composing", { text: "ni", cursor: 2 });
    expect.equal(c.text, "heXYnillo");
    expect.isTrue(c.value.composing.equals(new TextRange(4, 6)));
    await t.platformCall("momo/textinput", "composing", { text: "你", cursor: 1 });
    expect.equal(c.text, "heXY你llo");
    expect.isTrue(c.value.composing.equals(new TextRange(4, 5)));
    // 提交替换 composing
    await t.platformCall("momo/textinput", "commit", { text: "你好" });
    expect.equal(c.text, "heXY你好llo");
    expect.isFalse(c.value.isComposingRangeValid);
    expect.equal(c.selection.extentOffset, 6);
    // 取消预编辑:移除未提交文本
    await t.platformCall("momo/textinput", "composing", { text: "abc", cursor: 3 });
    await t.platformCall("momo/textinput", "composingEnd");
    expect.equal(c.text, "heXY你好llo");
    expect.equal(c.selection.extentOffset, 6);
    await t.pump();
    const last = [...calls].reverse().find((m) => m.method === "setEditingState")!;
    expect.deepEqual(last.args, { text: "heXY你好llo", selectionBase: 6, selectionExtent: 6, composingStart: -1, composingEnd: -1 });
  });

  testWidgets("commit/composing honour replaceStart/replaceEnd", async (t) => {
    const { c } = await focused(t, "resume");
    // 按住 e 选重音:输入法显式替换 [1,2) 为 "é"
    await t.platformCall("momo/textinput", "commit", { text: "é", replaceStart: 1, replaceEnd: 2 });
    expect.equal(c.text, "résume");
    expect.equal(c.selection.extentOffset, 2);
    // 重新转换:把 [0,2) 设为预编辑
    await t.platformCall("momo/textinput", "composing", { text: "RE", cursor: 2, replaceStart: 0, replaceEnd: 2 });
    expect.equal(c.text, "REsume");
    expect.isTrue(c.value.composing.equals(new TextRange(0, 2)));
    expect.equal(c.selection.extentOffset, 2);
    // 越界区间被钳制
    await t.platformCall("momo/textinput", "commit", { text: "!", replaceStart: 6, replaceEnd: 99 });
    expect.equal(c.text, "REsume!");
  });

  testWidgets("commands: delete, move, select, newline/submit", async (t) => {
    const submitted: string[] = [];
    const calls = t.recordMethodCalls("momo/textinput");
    void calls;
    const c = new TextEditingController({ text: "one two" });
    const n = new FocusNode();
    await t.pumpWidget(field(c, n, { onSubmitted: (s) => submitted.push(s) }));
    n.requestFocus();
    await t.pump();
    const cmd = (command: string): Promise<void> => t.platformCall("momo/textinput", "command", { command });
    await cmd("deleteBackward");
    expect.equal(c.text, "one tw");
    await cmd("moveWordLeft");
    expect.equal(c.selection.extentOffset, 4);
    await cmd("deleteWordBackward");
    expect.equal(c.text, "tw");
    await cmd("moveToEndOfLine");
    await cmd("moveLeftAndModifySelection");
    expect.deepEqual([c.selection.baseOffset, c.selection.extentOffset], [2, 1]);
    await cmd("deleteForward");
    expect.equal(c.text, "t");
    await cmd("selectAll");
    expect.deepEqual([c.selection.start, c.selection.end], [0, 1]);
    await cmd("insertNewline");
    expect.deepEqual(submitted, ["t"]);
    // 代理对整体删除
    await t.platformCall("momo/textinput", "commit", { text: "a😀" });
    await cmd("deleteBackward");
    expect.equal(c.text, "a");
  });

  testWidgets("keyboard navigation and editing keys", async (t) => {
    debugOverrideTargetPlatform("windows");
    try {
      const { c } = await focused(t, "ab cd", null);
      const left = key(LogicalKeyboardKey.arrowLeft, PhysicalKeyboardKey.arrowLeft);
      const home = key(LogicalKeyboardKey.home, PhysicalKeyboardKey.home);
      const back = key(LogicalKeyboardKey.backspace, PhysicalKeyboardKey.backspace);
      const enter = key(LogicalKeyboardKey.enter, PhysicalKeyboardKey.enter);
      expect.isTrue(await t.pressKey(left));
      expect.equal(c.selection.extentOffset, 4);
      await t.pressKey(left, KeyModifier.shift);
      expect.deepEqual([c.selection.baseOffset, c.selection.extentOffset], [4, 3]);
      await t.pressKey(back);
      expect.equal(c.text, "ab d");
      await t.pressKey(home);
      expect.equal(c.selection.extentOffset, 0);
      await t.pressKey(key(LogicalKeyboardKey.keyA, PhysicalKeyboardKey.keyA), KeyModifier.control);
      expect.deepEqual([c.selection.start, c.selection.end], [0, 4]);
      await t.pressKey(enter);
      expect.equal(c.text, "\n", "multiline Enter replaces selection with newline");
      // 预编辑时按键交给输入法(不处理)
      await t.platformCall("momo/textinput", "composing", { text: "x", cursor: 1 });
      expect.isFalse(await t.pressKey(left));
    } finally {
      debugOverrideTargetPlatform(null);
    }
  });

  testWidgets("multiline up/down and line start/end", async (t) => {
    const { c } = await focused(t, "abc\ndefgh\nij", null);
    c.selection = TextSelection.collapsed(6); // "de|fgh"
    await t.pump();
    await t.platformCall("momo/textinput", "command", { command: "moveUp" });
    expect.equal(c.selection.extentOffset, 2);
    await t.platformCall("momo/textinput", "command", { command: "moveDown" });
    await t.platformCall("momo/textinput", "command", { command: "moveDown" });
    expect.equal(c.selection.extentOffset, 12, "clamped to end of shorter line");
    await t.platformCall("momo/textinput", "command", { command: "moveToLineStart" });
    expect.equal(c.selection.extentOffset, 10);
  }, { ahemText: true });

  testWidgets("tap places the caret; double tap selects a word; drag selects", async (t) => {
    const c = new TextEditingController({ text: "hello world" });
    const n = new FocusNode();
    t.recordMethodCalls("momo/textinput");
    await t.pumpWidget(new Column({ children: [field(c, n)] }));
    // 字宽 10, 文本在 (0,0)
    await t.tap(new Offset(23, 5));
    expect.isTrue(n.hasFocus);
    expect.equal(c.selection.extentOffset, 2);
    await t.pump(400);
    await t.tap(new Offset(72, 5));
    await t.pump(50);
    await t.tap(new Offset(72, 5));
    expect.deepEqual([c.selection.start, c.selection.end], [6, 11]);
    await t.pump(400);
    await t.drag(new Offset(11, 5), new Offset(30, 0), { kind: Kind.mouse, steps: 3 });
    expect.deepEqual([c.selection.baseOffset, c.selection.extentOffset], [1, 4]);
  }, { ahemText: true });

  testWidgets("blur closes the connection (hide)", async (t) => {
    const { n, calls } = await focused(t, "x");
    n.unfocus();
    await t.pump();
    await t.flushMicrotasks();
    expect.equal(calls[calls.length - 1]!.method, "hide");
    void TextEditingValue;
  });

  testWidgets("cursor blinks via frame callbacks", async (t) => {
    const c = new TextEditingController({ text: "x" });
    const n = new FocusNode();
    t.recordMethodCalls("momo/textinput");
    await t.pumpWidget(new Center({ child: new SizedBox({ width: 100, child: new EditableText({ controller: c, focusNode: n }) }) }));
    n.requestFocus();
    await t.pump();
    const state = t.allElements().map((e) => (e as { state?: unknown }).state).find((s): s is { cursorCurrentlyVisible: boolean } => typeof s === "object" && s !== null && "cursorCurrentlyVisible" in s)!;
    expect.isTrue(state.cursorCurrentlyVisible);
    await t.pumpFor(560);
    expect.isFalse(state.cursorCurrentlyVisible);
    await t.pumpFor(520);
    expect.isTrue(state.cursorCurrentlyVisible);
  });
});
