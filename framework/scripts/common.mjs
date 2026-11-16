// 框架测试打包用的 esbuild 配置。打包方式本身(基础选项、momo:* 外部导入)与 `momo bundle` 是同一份
// (packages/cli/bundle_options.mjs);这里只加测试需要的: @momo/framework 解析到源码、momo:* 的 mock。
import path from "node:path";
import { fileURLToPath } from "node:url";

export { baseOptions, momoExternal } from "../packages/cli/bundle_options.mjs";

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

/** 测试用:momo:ui / momo:services → test/mock。 */
export const momoMock = {
  name: "momo-mock",
  setup(build) {
    build.onResolve({ filter: /^momo:(ui|services)$/ }, (args) => ({
      path: path.join(frameworkRoot, "test/mock", args.path === "momo:ui" ? "momo_ui.ts" : "momo_services.ts"),
    }));
  },
};
