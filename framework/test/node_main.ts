// Node 宿主:mock momo:ui / momo:services(由构建脚本别名注入)。
import { __mock } from "./mock/momo_ui";
import { runTests } from "./entry";

interface NodeProcess {
  exitCode?: number;
  argv: string[];
}
const proc = (globalThis as unknown as { process: NodeProcess }).process;
const filter = proc.argv[2];

const summary = await runTests({
  name: "node-mock",
  log: (line) => console.log(line),
  ahemText: true,
  mock: __mock,
  ...(filter ? { filter } : {}),
});
if (summary.failed > 0) proc.exitCode = 1;
