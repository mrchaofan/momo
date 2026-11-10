#!/usr/bin/env node
// 打包测试:
//   out/test/node_main.mjs  —— Node 宿主 + mock momo:ui/momo:services(npm test 运行它)
//   out/test/example_*.mjs  —— 在 mock 宿主下端到端运行 examples/counter、examples/todo、examples/webview(browser)
//   out/test/momo_tests.js  —— 引擎宿主:momo:* 保持外部导入, 导出 runTests(host)(供 tools/momo-test 调用)
import path from "node:path";
import * as esbuild from "esbuild";
import { baseOptions, frameworkRoot, momoExternal, momoFrameworkAlias, momoMock } from "./common.mjs";

// 示例依赖的本地扩展包: 只打包其 JS 门面(原生部分由 mock 通道代替)。
const localExtensions = {
  name: "momo-local-extensions",
  setup(build) {
    build.onResolve({ filter: /^@momo-examples\/ext-webview$/ }, () => ({
      path: path.join(frameworkRoot, "../extensions/examples/webview-extension/js/index.ts"),
    }));
  },
};

await Promise.all([
  esbuild.build({
    ...baseOptions,
    entryPoints: [path.join(frameworkRoot, "test/node_main.ts")],
    outfile: path.join(frameworkRoot, "out/test/node_main.mjs"),
    plugins: [momoMock, momoFrameworkAlias],
  }),
  ...["counter", "todo", "browser"].map((name) => esbuild.build({
    ...baseOptions,
    entryPoints: [path.join(frameworkRoot, `test/examples/${name}_main.ts`)],
    outfile: path.join(frameworkRoot, `out/test/example_${name}.mjs`),
    plugins: [momoMock, momoFrameworkAlias, localExtensions],
  })),
  esbuild.build({
    ...baseOptions,
    entryPoints: [path.join(frameworkRoot, "test/entry.ts")],
    outfile: path.join(frameworkRoot, "out/test/momo_tests.js"),
    plugins: [momoExternal, momoFrameworkAlias],
  }),
]);
