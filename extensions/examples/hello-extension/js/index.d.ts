export function add(a: number, b: number): number;
export function greet(name: string): string;
export function reverseBytes(bytes: Uint8Array): Uint8Array;
/** FNV-1a 64 位哈希(在后台线程计算), 返回 16 位十六进制字符串。 */
export function hash(bytes: Uint8Array): Promise<string>;
