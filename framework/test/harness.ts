// 极简测试运行器(不依赖 jest/node:test, 可在引擎 V8 内运行)。
import type { TestHost, TestSummary } from "./host";

export interface TestContext {
  readonly host: TestHost;
}

type TestBody = (ctx: TestContext) => void | Promise<void>;

interface TestCase {
  name: string;
  body: TestBody;
  mockOnly: boolean;
  ahemOnly: boolean;
}

const cases: TestCase[] = [];
const groupStack: string[] = [];

/** 分组(名称前缀)。 */
export function group(name: string, body: () => void): void {
  groupStack.push(name);
  try {
    body();
  } finally {
    groupStack.pop();
  }
}

/** 注册测试。opts.mockOnly:仅 mock 宿主;opts.ahemText:需要 Ahem 字形度量。 */
export function test(name: string, body: TestBody, opts: { mockOnly?: boolean; ahemText?: boolean } = {}): void {
  cases.push({
    name: [...groupStack, name].join(" › "),
    body,
    mockOnly: opts.mockOnly ?? false,
    ahemOnly: opts.ahemText ?? false,
  });
}

/** 测试前后的钩子(全局)。 */
const beforeEachHooks: (() => void)[] = [];
const afterEachHooks: (() => void | Promise<void>)[] = [];
export function beforeEach(fn: () => void): void {
  beforeEachHooks.push(fn);
}
export function afterEach(fn: () => void | Promise<void>): void {
  afterEachHooks.push(fn);
}

export class AssertionError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "AssertionError";
  }
}

function fmt(v: unknown): string {
  if (typeof v === "string") return JSON.stringify(v);
  if (v instanceof Uint8Array) return `Uint8Array[${Array.from(v).join(",")}]`;
  if (v instanceof Map) return `Map{${[...v.entries()].map(([k, x]) => `${fmt(k)}=>${fmt(x)}`).join(", ")}}`;
  if (Array.isArray(v)) return `[${v.map(fmt).join(", ")}]`;
  if (v !== null && typeof v === "object") {
    const ctor = (v as object).constructor;
    if (ctor === Object) return `{${Object.entries(v).map(([k, x]) => `${k}: ${fmt(x)}`).join(", ")}}`;
    return String(v);
  }
  return String(v);
}

function deepEqual(a: unknown, b: unknown): boolean {
  if (Object.is(a, b)) return true;
  if (typeof a !== typeof b || a === null || b === null || typeof a !== "object" || typeof b !== "object") return false;
  if (ArrayBuffer.isView(a) && ArrayBuffer.isView(b)) {
    if (a.constructor !== b.constructor) return false;
    const x = a as unknown as ArrayLike<unknown>;
    const y = b as unknown as ArrayLike<unknown>;
    if (x.length !== y.length) return false;
    for (let i = 0; i < x.length; i++) if (!Object.is(x[i], y[i])) return false;
    return true;
  }
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
    return a.every((v, i) => deepEqual(v, b[i]));
  }
  if (a instanceof Map || b instanceof Map) {
    if (!(a instanceof Map) || !(b instanceof Map) || a.size !== b.size) return false;
    for (const [k, v] of a) if (!b.has(k) || !deepEqual(v, b.get(k))) return false;
    return true;
  }
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  return ka.every((k) => deepEqual((a as Record<string, unknown>)[k], (b as Record<string, unknown>)[k]));
}

/** 断言工具。 */
export const expect = {
  equal<T>(actual: T, expected: T, message?: string): void {
    if (!Object.is(actual, expected)) {
      throw new AssertionError(`${message ? message + ": " : ""}expected ${fmt(expected)}, got ${fmt(actual)}`);
    }
  },
  notEqual<T>(actual: T, expected: T, message?: string): void {
    if (Object.is(actual, expected)) throw new AssertionError(`${message ? message + ": " : ""}expected value != ${fmt(expected)}`);
  },
  deepEqual(actual: unknown, expected: unknown, message?: string): void {
    if (!deepEqual(actual, expected)) {
      throw new AssertionError(`${message ? message + ": " : ""}expected ${fmt(expected)}, got ${fmt(actual)}`);
    }
  },
  close(actual: number, expected: number, epsilon = 1e-6, message?: string): void {
    if (!(Math.abs(actual - expected) <= epsilon)) {
      throw new AssertionError(`${message ? message + ": " : ""}expected ≈${expected} (±${epsilon}), got ${actual}`);
    }
  },
  isTrue(value: unknown, message?: string): void {
    if (value !== true) throw new AssertionError(message ?? `expected true, got ${fmt(value)}`);
  },
  isFalse(value: unknown, message?: string): void {
    if (value !== false) throw new AssertionError(message ?? `expected false, got ${fmt(value)}`);
  },
  isNull(value: unknown, message?: string): void {
    if (value !== null && value !== undefined) throw new AssertionError(message ?? `expected null, got ${fmt(value)}`);
  },
  notNull<T>(value: T | null | undefined, message?: string): T {
    if (value === null || value === undefined) throw new AssertionError(message ?? "expected non-null value");
    return value;
  },
  throws(fn: () => unknown, message?: string): void {
    try {
      fn();
    } catch {
      return;
    }
    throw new AssertionError(message ?? "expected function to throw");
  },
  async rejects(promise: Promise<unknown>, message?: string): Promise<unknown> {
    try {
      await promise;
    } catch (e) {
      return e;
    }
    throw new AssertionError(message ?? "expected promise to reject");
  },
};

/** 正在运行测试的宿主(WidgetTester 经它以宿主身份投递事件)。 */
export let currentHost: TestHost | null = null;

/** 运行全部已注册测试。 */
export async function runRegisteredTests(host: TestHost): Promise<TestSummary> {
  const summary: TestSummary = { passed: 0, failed: 0, skipped: 0, failures: [] };
  currentHost = host;
  for (const c of cases) {
    if (host.filter && !c.name.includes(host.filter)) continue;
    if ((c.mockOnly && host.mock === null) || (c.ahemOnly && !host.ahemText)) {
      summary.skipped++;
      host.log(`  skip  ${c.name}`);
      continue;
    }
    try {
      for (const h of beforeEachHooks) h();
      await c.body({ host });
      for (const h of afterEachHooks) await h();
      summary.passed++;
      host.log(`  ok    ${c.name}`);
    } catch (e) {
      try {
        for (const h of afterEachHooks) await h();
      } catch {
        // 忽略清理错误
      }
      const text = e instanceof Error ? `${e.message}\n${e.stack ?? ""}` : String(e);
      summary.failed++;
      summary.failures.push({ name: c.name, error: text });
      host.log(`  FAIL  ${c.name}\n${text.split("\n").map((l) => "        " + l).join("\n")}`);
    }
  }
  host.log(`\n${summary.passed} passed, ${summary.failed} failed, ${summary.skipped} skipped (host: ${host.name})`);
  return summary;
}
