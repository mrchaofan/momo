// 框架自己的 esbuild 配置: 构建运行时自带的模块(build_builtin.mjs)与测试打包(build_tests.mjs)共用。
import path from "node:path";
import { fileURLToPath } from "node:url";

/** 产物是单文件 ESM, 运行在引擎的 Node 环境里;momo:* 由运行时提供, 导入原样保留。 */
export const baseOptions = {
  bundle: true,
  format: "esm",
  platform: "neutral",
  target: "es2022",
  sourcemap: "inline",
  logLevel: "warning",
  legalComments: "none",
  mainFields: ["module", "main"],
  external: ["momo:*"],
};

export const frameworkRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const srcRoot = path.join(frameworkRoot, "packages/momo/src");
export const webviewRoot = path.join(frameworkRoot, "../extensions/webview");

/** 测试用: 运行时自带的 JS 模块解析到源码(插件的解析先于 external 的判定)。原生插件换成 test/mock 里的替身。 */
export const builtinFromSource = {
  name: "momo-builtin-from-source",
  setup(build) {
    build.onResolve({ filter: /^momo:framework$/ }, () => ({ path: path.join(srcRoot, "index.ts") }));
    build.onResolve({ filter: /^momo:webview$/ }, () => ({ path: path.join(webviewRoot, "js/index.ts") }));
    build.onResolve({ filter: /\/ext_webview\.node$/ }, () => ({ path: path.join(frameworkRoot, "test/mock/ext_webview.ts") }));
  },
};

/** 测试用:momo:ui / momo:services → test/mock。 */
export const momoMock = {
  name: "momo-mock",
  setup(build) {
    build.onResolve({ filter: /^momo:(ui|services)$/ }, (args) => ({
      path: path.join(frameworkRoot, "test/mock", args.path === "momo:ui" ? "momo_ui.ts" : "momo_services.ts"),
    }));
  },
};
