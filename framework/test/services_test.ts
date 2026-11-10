import type { KeyData } from "momo:ui";
import {
  AppLifecycle,
  BasicMessageChannel,
  Clipboard,
  HardwareKeyboard,
  JSONMessageCodec,
  KeyDownEvent,
  KeyRepeatEvent,
  KeyUpEvent,
  LogicalKeyboardKey,
  MethodCall,
  MethodChannel,
  MissingPluginException,
  PhysicalKeyboardKey,
  PlatformException,
  StandardMessageCodec,
  StandardMethodCodec,
  StringCodec,
  TextEditingValue,
  TextInput,
  TextRange,
  TextSelection,
  defaultBinaryMessenger,
  openUrl,
  utf8Decode,
  utf8Encode,
  type KeyEvent,
  type TextInputClient,
} from "../packages/momo/src/services/index";
import { expect, group, test as baseTest } from "./harness";

const codec = StandardMessageCodec.instance;
const methodCodec = StandardMethodCodec.instance;

function bytes(value: unknown): number[] {
  const b = codec.encodeMessage(value);
  return b === null ? [] : Array.from(new Uint8Array(b));
}

function buf(list: number[]): ArrayBuffer {
  return new Uint8Array(list).buffer;
}

function f64le(v: number): number[] {
  const dv = new DataView(new ArrayBuffer(8));
  dv.setFloat64(0, v, true);
  return Array.from(new Uint8Array(dv.buffer));
}

function roundTrip(value: unknown): unknown {
  return codec.decodeMessage(codec.encodeMessage(value));
}

/** 每个测试结束清理本文件用到的 mock handler(不影响其他测试注册的全局 handler)。 */
function cleanup(): void {
  for (const ch of ["test/basic", "test/method", "momo/clipboard", "momo/url", "momo/textinput"]) {
    defaultBinaryMessenger.setMockMessageHandler(ch, null);
  }
  TextInput.currentConnection?.close();
  TextInput.debugReset();
}

function test(name: string, body: () => void | Promise<void>): void {
  baseTest(name, async () => {
    try {
      await body();
    } finally {
      cleanup();
    }
  });
}

group("services › StandardMessageCodec bytes", () => {
  test("null encodes to no message, nested null to 0", () => {
    expect.isNull(codec.encodeMessage(null));
    expect.deepEqual(bytes([null]), [12, 1, 0]);
  });
  test("bool", () => {
    expect.deepEqual(bytes(true), [1]);
    expect.deepEqual(bytes(false), [2]);
  });
  test("int32", () => {
    expect.deepEqual(bytes(1), [3, 1, 0, 0, 0]);
    expect.deepEqual(bytes(-1), [3, 0xff, 0xff, 0xff, 0xff]);
    expect.deepEqual(bytes(2147483647), [3, 0xff, 0xff, 0xff, 0x7f]);
    expect.deepEqual(bytes(-2147483648), [3, 0, 0, 0, 0x80]);
  });
  test("int64 for safe integers outside int32", () => {
    expect.deepEqual(bytes(2147483648), [4, 0, 0, 0, 0x80, 0, 0, 0, 0]);
    expect.deepEqual(bytes(-2147483649), [4, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0xff]);
    expect.deepEqual(bytes(2n ** 62n), [4, 0, 0, 0, 0, 0, 0, 0, 0x40]);
    expect.equal(roundTrip(Number.MAX_SAFE_INTEGER), Number.MAX_SAFE_INTEGER);
    expect.equal(roundTrip(2n ** 62n), 2n ** 62n);
    expect.throws(() => codec.encodeMessage(2n ** 63n));
  });
  test("float64 with 8-byte alignment (relative to buffer start)", () => {
    expect.deepEqual(bytes(1.5), [6, 0, 0, 0, 0, 0, 0, 0, ...f64le(1.5)]);
    expect.deepEqual(bytes(-0), [6, 0, 0, 0, 0, 0, 0, 0, ...f64le(-0)]);
    expect.deepEqual(bytes(2 ** 60), [6, 0, 0, 0, 0, 0, 0, 0, ...f64le(2 ** 60)]);
    // list header (2 bytes) + type (1) → pad to 8
    expect.deepEqual(bytes([0.5]), [12, 1, 6, 0, 0, 0, 0, 0, ...f64le(0.5)]);
    // [1, 2.5]: 12,2, 3,1,0,0,0, 6 → offset 8 已对齐, 无填充
    expect.deepEqual(bytes([1, 2.5]), [12, 2, 3, 1, 0, 0, 0, 6, ...f64le(2.5)]);
    // [true, 2.5]: 12,2,1,6 → offset 4, 填充 4
    expect.deepEqual(bytes([true, 2.5]), [12, 2, 1, 6, 0, 0, 0, 0, ...f64le(2.5)]);
    expect.isTrue(Object.is(roundTrip(-0), -0));
    expect.equal(roundTrip(Math.PI), Math.PI);
    expect.isTrue(Number.isNaN(roundTrip(NaN) as number));
  });
  test("UTF-8 strings (multi-byte, surrogate pairs)", () => {
    expect.deepEqual(bytes(""), [7, 0]);
    expect.deepEqual(bytes("hi"), [7, 2, 0x68, 0x69]);
    expect.deepEqual(bytes("é"), [7, 2, 0xc3, 0xa9]);
    expect.deepEqual(bytes("中"), [7, 3, 0xe4, 0xb8, 0xad]);
    expect.deepEqual(bytes("😀"), [7, 4, 0xf0, 0x9f, 0x98, 0x80]);
    expect.equal(roundTrip("a中😀b"), "a中😀b");
    // lone surrogate → U+FFFD
    expect.deepEqual(Array.from(utf8Encode("\ud800")), [0xef, 0xbf, 0xbd]);
    expect.equal(utf8Decode(new Uint8Array([0x61, 0xff, 0x62])), "a�b");
    expect.equal(utf8Decode(new Uint8Array([0xe4, 0xb8])), "�");
  });
  test("size encoding boundaries 253/254/65535/65536", () => {
    const s253 = bytes("a".repeat(253));
    expect.deepEqual(s253.slice(0, 2), [7, 253]);
    expect.equal(s253.length, 255);
    const s254 = bytes("a".repeat(254));
    expect.deepEqual(s254.slice(0, 4), [7, 254, 254, 0]);
    expect.equal(s254.length, 4 + 254);
    const s65535 = bytes("a".repeat(65535));
    expect.deepEqual(s65535.slice(0, 4), [7, 254, 0xff, 0xff]);
    const s65536 = bytes("a".repeat(65536));
    expect.deepEqual(s65536.slice(0, 6), [7, 255, 0, 0, 1, 0]);
    expect.equal(s65536.length, 6 + 65536);
    expect.equal((roundTrip("b".repeat(65536)) as string).length, 65536);
  });
  test("Uint8Array / ArrayBuffer", () => {
    expect.deepEqual(bytes(new Uint8Array([1, 2, 3])), [8, 3, 1, 2, 3]);
    expect.deepEqual(bytes(new Uint8Array([9]).buffer), [8, 1, 9]);
    expect.deepEqual(roundTrip(new Uint8Array([4, 5])), new Uint8Array([4, 5]));
  });
  test("Int32Array aligned to 4", () => {
    // 9, size 2 → offset 2, pad 2 → 4
    expect.deepEqual(bytes(new Int32Array([1, -1])), [9, 2, 0, 0, 1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff]);
    expect.deepEqual(roundTrip(new Int32Array([7, -8, 9])), new Int32Array([7, -8, 9]));
  });
  test("Float64Array aligned to 8", () => {
    expect.deepEqual(bytes(new Float64Array([1])), [11, 1, 0, 0, 0, 0, 0, 0, ...f64le(1)]);
    // inside list: 12,1,11,1 → offset 4 → pad 4
    expect.deepEqual(bytes([new Float64Array([2])]), [12, 1, 11, 1, 0, 0, 0, 0, ...f64le(2)]);
    expect.deepEqual(roundTrip(new Float64Array([0.25, -3])), new Float64Array([0.25, -3]));
  });
  test("Float32Array / BigInt64Array", () => {
    expect.deepEqual(bytes(new Float32Array([1])), [14, 1, 0, 0, 0, 0, 0x80, 0x3f]);
    expect.deepEqual(roundTrip(new BigInt64Array([1n, -2n])), new BigInt64Array([1n, -2n]));
    expect.deepEqual(bytes(new BigInt64Array([1n])), [10, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]);
  });
  test("list and map", () => {
    expect.deepEqual(bytes([1, "a"]), [12, 2, 3, 1, 0, 0, 0, 7, 1, 0x61]);
    expect.deepEqual(bytes({ a: true }), [13, 1, 7, 1, 0x61, 1]);
    expect.deepEqual(bytes(new Map([[1, null]])), [13, 1, 3, 1, 0, 0, 0, 0]);
  });
  test("round trip nested structures", () => {
    const v = { name: "momo", list: [1, 2.5, "x", [true, null], { deep: new Uint8Array([1]) }], n: -5 };
    expect.deepEqual(roundTrip(v), v);
    const m = new Map<unknown, unknown>([[1, "one"], ["two", [2]]]);
    expect.deepEqual(roundTrip(m), m);
  });
  test("corrupted input throws", () => {
    expect.throws(() => codec.decodeMessage(buf([3, 1, 0])));
    expect.throws(() => codec.decodeMessage(buf([99])));
    expect.throws(() => codec.decodeMessage(buf([1, 1])));
  });
});

group("services › StandardMethodCodec", () => {
  test("method call is two consecutive values", () => {
    const b = Array.from(new Uint8Array(methodCodec.encodeMethodCall(new MethodCall("show", null))));
    expect.deepEqual(b, [7, 4, 0x73, 0x68, 0x6f, 0x77, 0]);
    const call = methodCodec.decodeMethodCall(methodCodec.encodeMethodCall(new MethodCall("x", { a: 1 })));
    expect.equal(call.method, "x");
    expect.deepEqual(call.args, { a: 1 });
  });
  test("lenient: method call as 2-element list", () => {
    const call = methodCodec.decodeMethodCall(codec.encodeMessage(["commit", { text: "a" }]));
    expect.equal(call.method, "commit");
    expect.deepEqual(call.args, { text: "a" });
  });
  test("success envelope", () => {
    expect.deepEqual(Array.from(new Uint8Array(methodCodec.encodeSuccessEnvelope(5))), [0, 3, 5, 0, 0, 0]);
    expect.equal(methodCodec.decodeEnvelope(methodCodec.encodeSuccessEnvelope("ok")), "ok");
    // float in envelope: aligned relative to buffer start (raw byte 0 + type 6 → pad 6)
    expect.deepEqual(Array.from(new Uint8Array(methodCodec.encodeSuccessEnvelope(0.5))), [0, 6, 0, 0, 0, 0, 0, 0, ...f64le(0.5)]);
  });
  test("error envelope", () => {
    const b = Array.from(new Uint8Array(methodCodec.encodeErrorEnvelope("E", "msg")));
    expect.deepEqual(b, [1, 7, 1, 0x45, 7, 3, 0x6d, 0x73, 0x67]);
    try {
      methodCodec.decodeEnvelope(methodCodec.encodeErrorEnvelope("E", "boom", [1]));
      throw new Error("not thrown");
    } catch (e) {
      expect.isTrue(e instanceof PlatformException);
      const pe = e as PlatformException;
      expect.equal(pe.code, "E");
      expect.equal(pe.platformMessage, "boom");
      expect.deepEqual(pe.details, [1]);
    }
  });
  test("lenient: list envelopes", () => {
    expect.equal(methodCodec.decodeEnvelope(codec.encodeMessage([0, "r"])!), "r");
    try {
      methodCodec.decodeEnvelope(codec.encodeMessage([1, "C", "m"])!);
      throw new Error("not thrown");
    } catch (e) {
      expect.isTrue(e instanceof PlatformException);
      expect.equal((e as PlatformException).code, "C");
    }
  });
});

group("services › channels", () => {
  test("other codecs", () => {
    expect.equal(StringCodec.instance.decodeMessage(StringCodec.instance.encodeMessage("中a")), "中a");
    expect.deepEqual(JSONMessageCodec.instance.decodeMessage(JSONMessageCodec.instance.encodeMessage({ a: [1] })), { a: [1] });
  });
  test("BasicMessageChannel both directions", async () => {
    const ch = new BasicMessageChannel<unknown>("test/basic", StandardMessageCodec.instance);
    defaultBinaryMessenger.setMockMessageHandler("test/basic", (data) => {
      const v = codec.decodeMessage(data);
      return codec.encodeMessage(["echo", v]);
    });
    expect.deepEqual(await ch.send(42), ["echo", 42]);
    let received: unknown = null;
    ch.setMessageHandler((m) => {
      received = m;
      return "pong";
    });
    const reply = await defaultBinaryMessenger.handlePlatformMessage("test/basic", codec.encodeMessage("ping"));
    expect.equal(received, "ping");
    expect.equal(codec.decodeMessage(reply), "pong");
    ch.setMessageHandler(null);
    expect.isNull(await defaultBinaryMessenger.handlePlatformMessage("test/basic", null));
  });
  test("MethodChannel invoke: success / error / missing", async () => {
    const ch = new MethodChannel("test/method");
    defaultBinaryMessenger.setMockMessageHandler("test/method", (data) => {
      const call = methodCodec.decodeMethodCall(data);
      if (call.method === "add") {
        const [a, b] = call.args as number[];
        return methodCodec.encodeSuccessEnvelope(a! + b!);
      }
      if (call.method === "missing") return null;
      return methodCodec.encodeErrorEnvelope("bad", `unknown ${call.method}`);
    });
    expect.equal(await ch.invokeMethod<number>("add", [2, 3]), 5);
    const e1 = await expect.rejects(ch.invokeMethod("nope"));
    expect.isTrue(e1 instanceof PlatformException);
    expect.equal((e1 as PlatformException).code, "bad");
    const e2 = await expect.rejects(ch.invokeMethod("missing"));
    expect.isTrue(e2 instanceof MissingPluginException);
  });
  test("MethodChannel handler: native → JS", async () => {
    const ch = new MethodChannel("test/method");
    ch.setMethodCallHandler((call) => {
      if (call.method === "double") return (call.args as number) * 2;
      if (call.method === "fail") throw new PlatformException("F", "failed");
      throw new Error("oops");
    });
    const r1 = await defaultBinaryMessenger.handlePlatformMessage("test/method", methodCodec.encodeMethodCall(new MethodCall("double", 21)));
    expect.equal(methodCodec.decodeEnvelope(r1!), 42);
    const r2 = await defaultBinaryMessenger.handlePlatformMessage("test/method", methodCodec.encodeMethodCall(new MethodCall("fail")));
    const e2 = (() => { try { methodCodec.decodeEnvelope(r2!); return null; } catch (e) { return e; } })();
    expect.equal((e2 as PlatformException).code, "F");
    const r3 = await defaultBinaryMessenger.handlePlatformMessage("test/method", methodCodec.encodeMethodCall(new MethodCall("x")));
    const e3 = (() => { try { methodCodec.decodeEnvelope(r3!); return null; } catch (e) { return e; } })();
    expect.equal((e3 as PlatformException).code, "error");
    ch.setMethodCallHandler(null);
  });
  test("Clipboard and openUrl", async () => {
    let stored: string | null = null;
    defaultBinaryMessenger.setMockMessageHandler("momo/clipboard", (data) => {
      const call = methodCodec.decodeMethodCall(data);
      if (call.method === "setText") {
        stored = call.args as string;
        return methodCodec.encodeSuccessEnvelope(null);
      }
      return methodCodec.encodeSuccessEnvelope(stored);
    });
    await Clipboard.setData("hello 世界");
    expect.equal(stored, "hello 世界");
    expect.equal(await Clipboard.getData(), "hello 世界");
    const opened: unknown[] = [];
    defaultBinaryMessenger.setMockMessageHandler("momo/url", (data) => {
      const call = methodCodec.decodeMethodCall(data);
      opened.push([call.method, call.args]);
      return methodCodec.encodeSuccessEnvelope(null);
    });
    await openUrl("https://example.com");
    expect.deepEqual(opened, [["open", "https://example.com"]]);
  });
  test("AppLifecycle (standard codec and raw utf-8)", async () => {
    const lc = AppLifecycle.instance;
    lc.ensureInitialized();
    const states: string[] = [];
    const listener = (s: string): void => void states.push(s);
    lc.addListener(listener);
    await defaultBinaryMessenger.handlePlatformMessage("momo/lifecycle", codec.encodeMessage("suspended"));
    await defaultBinaryMessenger.handlePlatformMessage("momo/lifecycle", StringCodec.instance.encodeMessage("resumed"));
    expect.deepEqual(states, ["suspended", "resumed"]);
    expect.equal(lc.state, "resumed");
    lc.removeListener(listener);
  });
});

group("services › text editing", () => {
  test("TextSelection / TextRange", () => {
    const s = new TextSelection(5, 2);
    expect.equal(s.start, 2);
    expect.equal(s.end, 5);
    expect.isFalse(s.isCollapsed);
    expect.isTrue(TextSelection.collapsed(3).equals(new TextSelection(3, 3)));
    expect.isFalse(TextRange.empty.isValid);
    expect.equal(new TextRange(1, 3).textInside("abcd"), "bc");
  });
  test("TextEditingValue.replaced adjusts selection", () => {
    const v = new TextEditingValue({ text: "hello", selection: TextSelection.collapsed(5), composing: new TextRange(0, 2) });
    const r = v.replaced(new TextRange(1, 3), "XYZ");
    expect.equal(r.text, "hXYZlo");
    expect.equal(r.selection.baseOffset, 6);
    expect.isFalse(r.composing.isValid);
  });
  test("TextInput: attach/show/setEditingState payloads and routing", async () => {
    const sent: [string, unknown][] = [];
    defaultBinaryMessenger.setMockMessageHandler("momo/textinput", (data) => {
      const call = methodCodec.decodeMethodCall(data);
      sent.push([call.method, call.args]);
      return methodCodec.encodeSuccessEnvelope(null);
    });
    const log: string[] = [];
    const client: TextInputClient = {
      commitText: (t) => log.push(`commit:${t}`),
      updateComposing: (t, c) => log.push(`composing:${t}:${c}`),
      endComposing: () => log.push("end"),
      performCommand: (c) => log.push(`cmd:${c}`),
      connectionClosed: () => log.push("closed"),
    };
    const conn = TextInput.attach(client);
    conn.show();
    conn.setEditingState(new TextEditingValue({ text: "ab", selection: new TextSelection(0, 2) }));
    conn.setCaretRect(1, 2, 3, 4);
    await Promise.resolve();
    await Promise.resolve();
    expect.deepEqual(sent, [
      ["show", null],
      ["setEditingState", { text: "ab", selectionBase: 0, selectionExtent: 2, composingStart: -1, composingEnd: -1 }],
      ["setCaretRect", [1, 2, 3, 4]],
    ]);
    const deliver = (method: string, args: unknown): Promise<ArrayBuffer | null> =>
      defaultBinaryMessenger.handlePlatformMessage("momo/textinput", methodCodec.encodeMethodCall(new MethodCall(method, args)));
    await deliver("composing", { text: "ni", cursor: 2 });
    await deliver("composingEnd", null);
    await deliver("commit", { text: "你" });
    await deliver("command", { command: "deleteBackward" });
    // list-form method call from native is accepted too
    await defaultBinaryMessenger.handlePlatformMessage("momo/textinput", codec.encodeMessage(["commit", { text: "x" }]));
    expect.deepEqual(log, ["composing:ni:2", "end", "commit:你", "cmd:deleteBackward", "commit:x"]);
    // re-attach closes the old connection
    const conn2 = TextInput.attach({ commitText: () => log.push("c2"), updateComposing: () => {}, endComposing: () => {}, performCommand: () => {} });
    expect.isFalse(conn.attached);
    expect.isTrue(conn2.attached);
    expect.equal(log[log.length - 1], "closed");
    await Promise.resolve();
    expect.equal(sent[sent.length - 1]![0], "hide");
    conn2.close();
    await deliver("commit", { text: "ignored" });
    expect.isFalse(log.includes("c2"));
  });
});

group("services › keyboard", () => {
  test("key constants", () => {
    expect.equal(LogicalKeyboardKey.keyA.keyId, 0x61);
    expect.equal(LogicalKeyboardKey.enter.keyId, 0x100070028);
    expect.equal(PhysicalKeyboardKey.keyA.keyId, 0x00070004);
    expect.equal(PhysicalKeyboardKey.enter.keyId, 0x00070028);
    expect.equal(LogicalKeyboardKey.findKeyByKeyId(0x61), LogicalKeyboardKey.keyA);
    expect.equal(LogicalKeyboardKey.findKeyByKeyId(0x4e2d), LogicalKeyboardKey.findKeyByKeyId(0x4e2d));
    expect.isTrue(LogicalKeyboardKey.isShift(LogicalKeyboardKey.shiftRight));
    expect.isFalse(LogicalKeyboardKey.isShift(LogicalKeyboardKey.controlLeft));
    const collapsed = LogicalKeyboardKey.collapseSynonyms([LogicalKeyboardKey.controlLeft, LogicalKeyboardKey.keyC]);
    expect.isTrue(collapsed.has(LogicalKeyboardKey.control) && collapsed.has(LogicalKeyboardKey.keyC));
  });
  test("HardwareKeyboard: conversion, pressed sets, modifiers, handlers", () => {
    const hk = HardwareKeyboard.instance;
    const events: KeyEvent[] = [];
    const h1 = (e: KeyEvent): boolean => {
      events.push(e);
      return false;
    };
    const h2 = (e: KeyEvent): boolean => e.logicalKey === LogicalKeyboardKey.keyA;
    hk.clearState();
    hk.addHandler(h1);
    hk.addHandler(h2);
    try {
    const kd = (type: KeyData["type"], physical: number, logical: number, character: string, modifiers: number): KeyData =>
      ({ type, timeStampUs: 2000, physicalKey: physical, logicalKey: logical, character, modifiers });
    expect.isFalse(hk.handleKeyData(kd("down", 0x700e1, LogicalKeyboardKey.shiftLeft.keyId, "", 1)));
    expect.isTrue(hk.isShiftPressed);
    expect.isTrue(hk.handleKeyData(kd("down", 0x70004, 0x61, "A", 1)));
    expect.isTrue(hk.logicalKeysPressed.has(LogicalKeyboardKey.keyA));
    expect.isTrue(hk.physicalKeysPressed.has(PhysicalKeyboardKey.keyA));
    expect.equal(hk.lookUpLayout(PhysicalKeyboardKey.keyA), LogicalKeyboardKey.keyA);
    hk.handleKeyData(kd("repeat", 0x70004, 0x61, "A", 1));
    hk.handleKeyData(kd("up", 0x70004, 0x61, "", 1));
    hk.handleKeyData(kd("up", 0x700e1, LogicalKeyboardKey.shiftLeft.keyId, "", 0));
    expect.isFalse(hk.isShiftPressed);
    expect.equal(hk.logicalKeysPressed.size, 0);
    expect.equal(events.length, 5);
    expect.isTrue(events[1] instanceof KeyDownEvent);
    expect.equal(events[1]!.character, "A");
    expect.equal(events[1]!.timeStamp, 2);
    expect.isTrue(events[2] instanceof KeyRepeatEvent);
    expect.isTrue(events[3] instanceof KeyUpEvent);
    expect.isNull(events[0]!.character);
    } finally {
      hk.removeHandler(h1);
      hk.removeHandler(h2);
      hk.clearState();
    }
  });
});
