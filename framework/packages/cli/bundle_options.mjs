// Momo 应用的 JS 怎么打包: 这一份设置由 `momo bundle` 与框架自己的测试打包(framework/scripts)共用。
// 产物是单文件 ESM, 运行在引擎的 Node 环境里;momo:* 是引擎提供的原生模块, 保持为外部导入。

/** esbuild 的基础选项。 */
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

/** 引擎原生模块: 保留为 `import ... from "momo:ui"`。 */
export const momoExternal = {
  name: "momo-external",
  setup(build) {
    build.onResolve({ filter: /^momo:/ }, (args) => ({ path: args.path, external: true }));
  },
};
