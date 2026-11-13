import { test } from "./harness";
import type { TestContext } from "./harness";
import { resetTester, tester, type ViewTester } from "./tester";
import { Size } from "../packages/momo/src/index";

/** view 测试:提供 tester, 结束后复位框架状态。 */
export function testViews(name: string, body: (t: ViewTester, ctx: TestContext) => Promise<void> | void,
  opts: { mockOnly?: boolean; ahemText?: boolean; size?: Size; dpr?: number } = {}): void {
  test(name, async (ctx) => {
    const t = tester(opts.size ?? new Size(800, 600), opts.dpr ?? 1);
    try {
      await body(t, ctx);
    } finally {
      await resetTester();
    }
  }, opts);
}
