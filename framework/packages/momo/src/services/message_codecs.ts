/** 一次方法调用。 */
export class MethodCall {
  constructor(readonly method: string, readonly args: unknown = null) {}
}

/** 平台侧返回的错误(错误信封)。 */
export class PlatformException extends Error {
  constructor(readonly code: string, message: string | null = null, readonly details: unknown = null) {
    super(message ?? code);
    this.name = "PlatformException";
    this.platformMessage = message;
  }
  /** 原生侧给出的 message(可空)。 */
  readonly platformMessage: string | null;
}

/** 通道上没有原生 handler(回复为 null)。 */
export class MissingPluginException extends Error {
  constructor(message: string) {
    super(message);
    this.name = "MissingPluginException";
  }
}

/** 消息编解码器。 */
export interface MessageCodec<T> {
  encodeMessage(message: T | null): ArrayBuffer | null;
  decodeMessage(data: ArrayBuffer | null): T | null;
}

/** 方法调用编解码器。 */
export interface MethodCodec {
  encodeMethodCall(call: MethodCall): ArrayBuffer;
  decodeMethodCall(data: ArrayBuffer | null): MethodCall;
  encodeSuccessEnvelope(result: unknown): ArrayBuffer;
  encodeErrorEnvelope(code: string, message?: string | null, details?: unknown): ArrayBuffer;
  /** 成功返回结果, 错误抛 {@link PlatformException}。 */
  decodeEnvelope(data: ArrayBuffer): unknown;
}

/** UTF-16 字符串 → UTF-8 字节(孤立代理替换为 U+FFFD)。 */
export function utf8Encode(s: string): Uint8Array {
  const n = s.length;
  let size = 0;
  for (let i = 0; i < n; i++) {
    let c = s.charCodeAt(i);
    if (c < 0x80) size += 1;
    else if (c < 0x800) size += 2;
    else if (c >= 0xd800 && c <= 0xdbff && i + 1 < n) {
      const d = s.charCodeAt(i + 1);
      if (d >= 0xdc00 && d <= 0xdfff) {
        size += 4;
        i++;
      } else size += 3;
    } else size += 3;
    c = 0;
  }
  const out = new Uint8Array(size);
  let p = 0;
  for (let i = 0; i < n; i++) {
    let c = s.charCodeAt(i);
    if (c < 0x80) {
      out[p++] = c;
      continue;
    }
    if (c < 0x800) {
      out[p++] = 0xc0 | (c >> 6);
      out[p++] = 0x80 | (c & 0x3f);
      continue;
    }
    if (c >= 0xd800 && c <= 0xdfff) {
      const d = i + 1 < n ? s.charCodeAt(i + 1) : 0;
      if (c <= 0xdbff && d >= 0xdc00 && d <= 0xdfff) {
        const cp = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
        out[p++] = 0xf0 | (cp >> 18);
        out[p++] = 0x80 | ((cp >> 12) & 0x3f);
        out[p++] = 0x80 | ((cp >> 6) & 0x3f);
        out[p++] = 0x80 | (cp & 0x3f);
        i++;
        continue;
      }
      c = 0xfffd;
    }
    out[p++] = 0xe0 | (c >> 12);
    out[p++] = 0x80 | ((c >> 6) & 0x3f);
    out[p++] = 0x80 | (c & 0x3f);
  }
  return out;
}

/** UTF-8 字节 → 字符串(非法序列替换为 U+FFFD)。 */
export function utf8Decode(bytes: Uint8Array): string {
  const n = bytes.length;
  let out = "";
  const chunk: number[] = [];
  let i = 0;
  const flush = (): void => {
    out += String.fromCharCode.apply(null, chunk);
    chunk.length = 0;
  };
  while (i < n) {
    const b0 = bytes[i]!;
    let cp = 0xfffd;
    let len = 1;
    if (b0 < 0x80) {
      cp = b0;
    } else if (b0 >= 0xc2 && b0 <= 0xdf) {
      const b1 = bytes[i + 1];
      if (b1 !== undefined && (b1 & 0xc0) === 0x80) {
        cp = ((b0 & 0x1f) << 6) | (b1 & 0x3f);
        len = 2;
      }
    } else if (b0 >= 0xe0 && b0 <= 0xef) {
      const b1 = bytes[i + 1];
      const b2 = bytes[i + 2];
      const lo = b0 === 0xe0 ? 0xa0 : 0x80;
      const hi = b0 === 0xed ? 0x9f : 0xbf;
      if (b1 !== undefined && b1 >= lo && b1 <= hi) {
        if (b2 !== undefined && (b2 & 0xc0) === 0x80) {
          cp = ((b0 & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
          len = 3;
        } else len = 2;
      }
    } else if (b0 >= 0xf0 && b0 <= 0xf4) {
      const b1 = bytes[i + 1];
      const b2 = bytes[i + 2];
      const b3 = bytes[i + 3];
      const lo = b0 === 0xf0 ? 0x90 : 0x80;
      const hi = b0 === 0xf4 ? 0x8f : 0xbf;
      if (b1 !== undefined && b1 >= lo && b1 <= hi) {
        if (b2 !== undefined && (b2 & 0xc0) === 0x80) {
          if (b3 !== undefined && (b3 & 0xc0) === 0x80) {
            cp = ((b0 & 0x07) << 18) | ((b1 & 0x3f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f);
            len = 4;
          } else len = 3;
        } else len = 2;
      }
    }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      chunk.push(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
    } else chunk.push(cp);
    if (chunk.length >= 4096) flush();
  }
  flush();
  return out;
}

function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  if (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength && bytes.buffer instanceof ArrayBuffer) {
    return bytes.buffer;
  }
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

/** UTF-8 字符串编解码器。 */
export class StringCodec implements MessageCodec<string> {
  static readonly instance = new StringCodec();
  encodeMessage(message: string | null): ArrayBuffer | null {
    return message === null ? null : toArrayBuffer(utf8Encode(message));
  }
  decodeMessage(data: ArrayBuffer | null): string | null {
    return data === null ? null : utf8Decode(new Uint8Array(data));
  }
}

/** 原样传递字节。 */
export class BinaryCodec implements MessageCodec<ArrayBuffer> {
  static readonly instance = new BinaryCodec();
  encodeMessage(message: ArrayBuffer | null): ArrayBuffer | null {
    return message;
  }
  decodeMessage(data: ArrayBuffer | null): ArrayBuffer | null {
    return data;
  }
}

/** JSON(UTF-8)编解码器。 */
export class JSONMessageCodec implements MessageCodec<unknown> {
  static readonly instance = new JSONMessageCodec();
  encodeMessage(message: unknown): ArrayBuffer | null {
    if (message === null || message === undefined) return null;
    return toArrayBuffer(utf8Encode(JSON.stringify(message)));
  }
  decodeMessage(data: ArrayBuffer | null): unknown {
    if (data === null) return null;
    return JSON.parse(utf8Decode(new Uint8Array(data))) as unknown;
  }
}
