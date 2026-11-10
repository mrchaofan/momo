// StandardMessageCodec / StandardMethodCodec —— 字节格式与 Flutter 完全一致(C++ 侧 engine/runtime/standard_codec.h 同格式)。
import {
  MethodCall,
  PlatformException,
  utf8Decode,
  utf8Encode,
  type MessageCodec,
  type MethodCodec,
} from "./message_codecs";

/** 可增长的小端写缓冲。对齐以缓冲起点为基准。 */
export class WriteBuffer {
  #bytes = new Uint8Array(64);
  #view = new DataView(this.#bytes.buffer);
  #length = 0;

  get length(): number {
    return this.#length;
  }

  #ensure(extra: number): void {
    const need = this.#length + extra;
    if (need <= this.#bytes.length) return;
    let cap = this.#bytes.length * 2;
    while (cap < need) cap *= 2;
    const next = new Uint8Array(cap);
    next.set(this.#bytes.subarray(0, this.#length));
    this.#bytes = next;
    this.#view = new DataView(next.buffer);
  }

  putUint8(v: number): void {
    this.#ensure(1);
    this.#bytes[this.#length++] = v & 0xff;
  }
  putUint16(v: number): void {
    this.#ensure(2);
    this.#view.setUint16(this.#length, v, true);
    this.#length += 2;
  }
  putUint32(v: number): void {
    this.#ensure(4);
    this.#view.setUint32(this.#length, v, true);
    this.#length += 4;
  }
  putInt32(v: number): void {
    this.#ensure(4);
    this.#view.setInt32(this.#length, v, true);
    this.#length += 4;
  }
  putInt64(v: bigint): void {
    this.#ensure(8);
    this.#view.setBigInt64(this.#length, v, true);
    this.#length += 8;
  }
  /** 先按 8 字节对齐再写。 */
  putFloat64(v: number): void {
    this.alignTo(8);
    this.#ensure(8);
    this.#view.setFloat64(this.#length, v, true);
    this.#length += 8;
  }
  putUint8List(list: Uint8Array): void {
    this.#ensure(list.length);
    this.#bytes.set(list, this.#length);
    this.#length += list.length;
  }
  /** 写入任意 TypedArray 的原始字节(调用方负责对齐;平台为小端)。 */
  putRawView(view: ArrayBufferView): void {
    this.putUint8List(new Uint8Array(view.buffer, view.byteOffset, view.byteLength));
  }
  /** 填 0 直到长度是 alignment 的倍数。 */
  alignTo(alignment: number): void {
    const mod = this.#length % alignment;
    if (mod !== 0) {
      const pad = alignment - mod;
      this.#ensure(pad);
      for (let i = 0; i < pad; i++) this.#bytes[this.#length++] = 0;
    }
  }
  /** 取出结果(拷贝为精确长度的 ArrayBuffer)。 */
  done(): ArrayBuffer {
    return this.#bytes.slice(0, this.#length).buffer;
  }
}

/** 小端读缓冲。 */
export class ReadBuffer {
  readonly data: DataView;
  #pos = 0;
  constructor(data: ArrayBuffer | DataView) {
    this.data = data instanceof DataView ? data : new DataView(data);
  }
  get position(): number {
    return this.#pos;
  }
  get hasRemaining(): boolean {
    return this.#pos < this.data.byteLength;
  }
  #need(n: number): void {
    if (this.#pos + n > this.data.byteLength) throw new RangeError("Message corrupted: unexpected end of buffer");
  }
  getUint8(): number {
    this.#need(1);
    return this.data.getUint8(this.#pos++);
  }
  getUint16(): number {
    this.#need(2);
    const v = this.data.getUint16(this.#pos, true);
    this.#pos += 2;
    return v;
  }
  getUint32(): number {
    this.#need(4);
    const v = this.data.getUint32(this.#pos, true);
    this.#pos += 4;
    return v;
  }
  getInt32(): number {
    this.#need(4);
    const v = this.data.getInt32(this.#pos, true);
    this.#pos += 4;
    return v;
  }
  getInt64(): bigint {
    this.#need(8);
    const v = this.data.getBigInt64(this.#pos, true);
    this.#pos += 8;
    return v;
  }
  getFloat64(): number {
    this.alignTo(8);
    this.#need(8);
    const v = this.data.getFloat64(this.#pos, true);
    this.#pos += 8;
    return v;
  }
  /** 拷贝出 n 字节。 */
  getUint8List(n: number): Uint8Array {
    this.#need(n);
    const start = this.data.byteOffset + this.#pos;
    this.#pos += n;
    return new Uint8Array(this.data.buffer.slice(start, start + n));
  }
  alignTo(alignment: number): void {
    const mod = this.#pos % alignment;
    if (mod !== 0) this.#pos += alignment - mod;
  }
}

const enum T {
  null_ = 0,
  true_ = 1,
  false_ = 2,
  int32 = 3,
  int64 = 4,
  float64 = 6,
  string = 7,
  uint8List = 8,
  int32List = 9,
  int64List = 10,
  float64List = 11,
  list = 12,
  map = 13,
  float32List = 14,
}

const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;
const INT64_MIN = -(1n << 63n);
const INT64_MAX = (1n << 63n) - 1n;

/**
 * 与 Flutter StandardMessageCodec 字节兼容的编解码器。
 * number:int32 范围整数 → int32;其他安全整数 → int64;否则(含 -0)→ float64。
 * map 解码:key 全为字符串时返回普通对象, 否则返回 Map;int64 超出安全整数范围时解码为 bigint。
 */
export class StandardMessageCodec implements MessageCodec<unknown> {
  static readonly instance = new StandardMessageCodec();

  encodeMessage(value: unknown): ArrayBuffer | null {
    if (value === null || value === undefined) return null;
    const buffer = new WriteBuffer();
    this.writeValue(buffer, value);
    return buffer.done();
  }

  decodeMessage(data: ArrayBuffer | null): unknown {
    if (data === null) return null;
    const buffer = new ReadBuffer(data);
    const result = this.readValue(buffer);
    if (buffer.hasRemaining) throw new Error("Message corrupted: trailing bytes");
    return result;
  }

  /** 写长度(<254 一字节, ≤0xffff 为 254 + uint16, 否则 255 + uint32)。 */
  writeSize(buffer: WriteBuffer, size: number): void {
    if (size < 254) buffer.putUint8(size);
    else if (size <= 0xffff) {
      buffer.putUint8(254);
      buffer.putUint16(size);
    } else {
      buffer.putUint8(255);
      buffer.putUint32(size);
    }
  }

  readSize(buffer: ReadBuffer): number {
    const v = buffer.getUint8();
    if (v < 254) return v;
    if (v === 254) return buffer.getUint16();
    return buffer.getUint32();
  }

  writeValue(buffer: WriteBuffer, value: unknown): void {
    if (value === null || value === undefined) {
      buffer.putUint8(T.null_);
    } else if (value === true) {
      buffer.putUint8(T.true_);
    } else if (value === false) {
      buffer.putUint8(T.false_);
    } else if (typeof value === "number") {
      if (Number.isInteger(value) && !Object.is(value, -0)) {
        if (value >= INT32_MIN && value <= INT32_MAX) {
          buffer.putUint8(T.int32);
          buffer.putInt32(value);
          return;
        }
        if (Number.isSafeInteger(value)) {
          buffer.putUint8(T.int64);
          buffer.putInt64(BigInt(value));
          return;
        }
      }
      buffer.putUint8(T.float64);
      buffer.putFloat64(value);
    } else if (typeof value === "bigint") {
      if (value < INT64_MIN || value > INT64_MAX) throw new RangeError(`bigint out of int64 range: ${value}`);
      buffer.putUint8(T.int64);
      buffer.putInt64(value);
    } else if (typeof value === "string") {
      buffer.putUint8(T.string);
      const bytes = utf8Encode(value);
      this.writeSize(buffer, bytes.length);
      buffer.putUint8List(bytes);
    } else if (value instanceof Uint8Array) {
      buffer.putUint8(T.uint8List);
      this.writeSize(buffer, value.length);
      buffer.putUint8List(value);
    } else if (value instanceof ArrayBuffer) {
      buffer.putUint8(T.uint8List);
      this.writeSize(buffer, value.byteLength);
      buffer.putUint8List(new Uint8Array(value));
    } else if (value instanceof Int32Array) {
      buffer.putUint8(T.int32List);
      this.writeSize(buffer, value.length);
      buffer.alignTo(4);
      buffer.putRawView(value);
    } else if (value instanceof BigInt64Array) {
      buffer.putUint8(T.int64List);
      this.writeSize(buffer, value.length);
      buffer.alignTo(8);
      buffer.putRawView(value);
    } else if (value instanceof Float64Array) {
      buffer.putUint8(T.float64List);
      this.writeSize(buffer, value.length);
      buffer.alignTo(8);
      buffer.putRawView(value);
    } else if (value instanceof Float32Array) {
      buffer.putUint8(T.float32List);
      this.writeSize(buffer, value.length);
      buffer.alignTo(4);
      buffer.putRawView(value);
    } else if (Array.isArray(value)) {
      buffer.putUint8(T.list);
      this.writeSize(buffer, value.length);
      for (const item of value as unknown[]) this.writeValue(buffer, item);
    } else if (value instanceof Map) {
      buffer.putUint8(T.map);
      this.writeSize(buffer, value.size);
      for (const [k, v] of value as Map<unknown, unknown>) {
        this.writeValue(buffer, k);
        this.writeValue(buffer, v);
      }
    } else if (typeof value === "object") {
      const entries = Object.entries(value as Record<string, unknown>);
      buffer.putUint8(T.map);
      this.writeSize(buffer, entries.length);
      for (const [k, v] of entries) {
        this.writeValue(buffer, k);
        this.writeValue(buffer, v);
      }
    } else {
      throw new TypeError(`StandardMessageCodec: unsupported value of type ${typeof value}`);
    }
  }

  readValue(buffer: ReadBuffer): unknown {
    return this.readValueOfType(buffer.getUint8(), buffer);
  }

  readValueOfType(type: number, buffer: ReadBuffer): unknown {
    switch (type) {
      case T.null_:
        return null;
      case T.true_:
        return true;
      case T.false_:
        return false;
      case T.int32:
        return buffer.getInt32();
      case T.int64: {
        const v = buffer.getInt64();
        return v >= BigInt(Number.MIN_SAFE_INTEGER) && v <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(v) : v;
      }
      case T.float64:
        return buffer.getFloat64();
      case T.string: {
        const n = this.readSize(buffer);
        return utf8Decode(buffer.getUint8List(n));
      }
      case T.uint8List: {
        const n = this.readSize(buffer);
        return buffer.getUint8List(n);
      }
      case T.int32List: {
        const n = this.readSize(buffer);
        buffer.alignTo(4);
        return new Int32Array(buffer.getUint8List(n * 4).buffer);
      }
      case T.int64List: {
        const n = this.readSize(buffer);
        buffer.alignTo(8);
        return new BigInt64Array(buffer.getUint8List(n * 8).buffer);
      }
      case T.float64List: {
        const n = this.readSize(buffer);
        buffer.alignTo(8);
        return new Float64Array(buffer.getUint8List(n * 8).buffer);
      }
      case T.float32List: {
        const n = this.readSize(buffer);
        buffer.alignTo(4);
        return new Float32Array(buffer.getUint8List(n * 4).buffer);
      }
      case T.list: {
        const n = this.readSize(buffer);
        const out: unknown[] = new Array<unknown>(n);
        for (let i = 0; i < n; i++) out[i] = this.readValue(buffer);
        return out;
      }
      case T.map: {
        const n = this.readSize(buffer);
        const keys: unknown[] = [];
        const values: unknown[] = [];
        let allStrings = true;
        for (let i = 0; i < n; i++) {
          const k = this.readValue(buffer);
          if (typeof k !== "string") allStrings = false;
          keys.push(k);
          values.push(this.readValue(buffer));
        }
        if (allStrings) {
          const obj: Record<string, unknown> = {};
          for (let i = 0; i < n; i++) obj[keys[i] as string] = values[i];
          return obj;
        }
        const map = new Map<unknown, unknown>();
        for (let i = 0; i < n; i++) map.set(keys[i], values[i]);
        return map;
      }
      default:
        throw new Error(`Message corrupted: unknown type byte ${type}`);
    }
  }
}

/**
 * 与 Flutter StandardMethodCodec 字节兼容:
 *   请求 = 连续两个值 method、args;成功信封 = 原始字节 0 + result;
 *   错误信封 = 原始字节 1 + code + message (+ details, 非 null 时)。
 * 解码宽容:接受 list 形式的请求 [method, args] 与信封 [0, result] / [1, code, message, details?]。
 */
export class StandardMethodCodec implements MethodCodec {
  static readonly instance = new StandardMethodCodec();

  constructor(readonly messageCodec: StandardMessageCodec = StandardMessageCodec.instance) {}

  encodeMethodCall(call: MethodCall): ArrayBuffer {
    const buffer = new WriteBuffer();
    this.messageCodec.writeValue(buffer, call.method);
    this.messageCodec.writeValue(buffer, call.args);
    return buffer.done();
  }

  decodeMethodCall(data: ArrayBuffer | null): MethodCall {
    if (data === null) throw new Error("Invalid method call: null");
    const buffer = new ReadBuffer(data);
    const first = this.messageCodec.readValue(buffer);
    if (typeof first === "string") {
      const args = buffer.hasRemaining ? this.messageCodec.readValue(buffer) : null;
      if (buffer.hasRemaining) throw new Error("Invalid method call: trailing bytes");
      return new MethodCall(first, args);
    }
    if (Array.isArray(first) && first.length >= 1 && first.length <= 2 && typeof first[0] === "string" && !buffer.hasRemaining) {
      return new MethodCall(first[0], first.length === 2 ? (first[1] as unknown) : null);
    }
    throw new Error("Invalid method call");
  }

  encodeSuccessEnvelope(result: unknown): ArrayBuffer {
    const buffer = new WriteBuffer();
    buffer.putUint8(0);
    this.messageCodec.writeValue(buffer, result);
    return buffer.done();
  }

  encodeErrorEnvelope(code: string, message?: string | null, details?: unknown): ArrayBuffer {
    const buffer = new WriteBuffer();
    buffer.putUint8(1);
    this.messageCodec.writeValue(buffer, code);
    this.messageCodec.writeValue(buffer, message ?? null);
    if (details !== null && details !== undefined) this.messageCodec.writeValue(buffer, details);
    return buffer.done();
  }

  decodeEnvelope(data: ArrayBuffer): unknown {
    const buffer = new ReadBuffer(data);
    if (!buffer.hasRemaining) throw new Error("Expected envelope, got nothing");
    const tag = buffer.getUint8();
    if (tag === 0) {
      const result = buffer.hasRemaining ? this.messageCodec.readValue(buffer) : null;
      if (buffer.hasRemaining) throw new Error("Invalid envelope: trailing bytes");
      return result;
    }
    if (tag === 1) {
      const code = this.messageCodec.readValue(buffer);
      const message = buffer.hasRemaining ? this.messageCodec.readValue(buffer) : null;
      const details = buffer.hasRemaining ? this.messageCodec.readValue(buffer) : null;
      throw StandardMethodCodec.#makeError(code, message, details);
    }
    if (tag === 12) {
      const list = this.messageCodec.readValueOfType(tag, buffer);
      if (Array.isArray(list) && list.length >= 1) {
        if (list[0] === 0 && list.length <= 2) return list.length === 2 ? (list[1] as unknown) : null;
        if (list[0] === 1 && list.length >= 2 && list.length <= 4) {
          throw StandardMethodCodec.#makeError(list[1], list[2] ?? null, list[3] ?? null);
        }
      }
    }
    throw new Error("Invalid envelope");
  }

  static #makeError(code: unknown, message: unknown, details: unknown): PlatformException {
    if (typeof code !== "string") throw new Error("Invalid error envelope: code is not a string");
    if (message !== null && typeof message !== "string") throw new Error("Invalid error envelope: message is not a string");
    return new PlatformException(code, message, details);
  }
}
