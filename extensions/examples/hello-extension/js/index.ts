// @momo-examples/ext-hello 的 JS 门面: 给插件的函数加上类型。
import addon from "../build/ext_hello.node";

const native = addon as {
  add(a: number, b: number): number;
  greet(name: string): string;
  reverseBytes(bytes: Uint8Array): Uint8Array;
  hash(bytes: Uint8Array): Promise<string>;
};

export function add(a: number, b: number): number {
  return native.add(a, b);
}

export function greet(name: string): string {
  return native.greet(name);
}

export function reverseBytes(bytes: Uint8Array): Uint8Array {
  return native.reverseBytes(bytes);
}

/** FNV-1a 64 位哈希(在后台线程计算), 返回 16 位十六进制字符串。 */
export function hash(bytes: Uint8Array): Promise<string> {
  return native.hash(bytes);
}
