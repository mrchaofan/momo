// @momo-examples/ext-hello 的 JS 门面: 把 ext:hello 的原生函数包装成类型友好的 API。
import { native } from "ext:hello";
import { send } from "momo:services";

type Fn = (...args: unknown[]) => unknown;
const fns = native as unknown as Record<string, Fn>;

export function add(a: number, b: number): number {
  return fns.add(a, b) as number;
}

export function greet(name: string): string {
  return fns.greet(name) as string;
}

export function reverseBytes(bytes: Uint8Array): Uint8Array {
  return new Uint8Array(fns.reverseBytes(bytes) as ArrayBuffer);
}

/** FNV-1a 64 位哈希(在扩展的 worker 线程计算), 返回 16 进制字符串。 */
export async function hash(bytes: Uint8Array): Promise<string> {
  const out = new Uint8Array((await (fns.hashAsync(bytes) as Promise<ArrayBuffer>)));
  let hex = "";
  for (let i = out.length - 1; i >= 0; i--) hex += out[i].toString(16).padStart(2, "0");
  return hex;
}

/** 经消息通道 hello/upper 把 ASCII 文本转大写。 */
export async function upper(text: string): Promise<string> {
  const bytes = new Uint8Array([...text].map((c) => c.charCodeAt(0) & 0xff));
  const reply = await send("hello/upper", bytes.buffer);
  return reply ? String.fromCharCode(...new Uint8Array(reply)) : "";
}
