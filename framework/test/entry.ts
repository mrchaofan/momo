// 测试 bundle 入口:任何宿主都调用 runTests(host)。
import "./all_tests";
import { runRegisteredTests } from "./harness";
import type { TestHost, TestSummary } from "./host";

export type { TestHost, TestSummary, MockControl } from "./host";

/** 运行全部框架测试。 */
export function runTests(host: TestHost): Promise<TestSummary> {
  return runRegisteredTests(host);
}
