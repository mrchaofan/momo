// 共享 esbuild 配置:momo:* 保持为外部 ESM 导入, @momo/framework 解析到源码。
import path from "node:path";
import { fileURLToPath } from "node:url";

export const frameworkRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const srcRoot = path.join(frameworkRoot, "packages/momo/src");

/** @momo/framework[/sub] → packages/momo/src[/sub]/index.ts */
export const momoFrameworkAlias = {
  name: "momo-framework-alias",
  setup(build) {
    build.onResolve({ filter: /^@momo\/framework(\/.*)?$/ }, (args) => {
      const sub = args.path.slice("@momo/framework".length);
      return { path: path.join(srcRoot, sub === "" ? "index.ts" : `${sub.slice(1)}/index.ts`) };
    });
  },
};

/** 引擎原生模块:保留为 `import ... from "momo:ui"`。 */
export const momoExternal = {
  name: "momo-external",
  setup(build) {
    build.onResolve({ filter: /^(momo|ext):/ }, (args) => ({ path: args.path, external: true }));
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

export const baseOptions = {
  bundle: true,
  format: "esm",
  platform: "neutral",
  target: "es2022",
  sourcemap: "inline",
  logLevel: "warning",
  legalComments: "none",
  mainFields: ["module", "main"],
};
