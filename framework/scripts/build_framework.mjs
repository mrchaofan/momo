#!/usr/bin/env node
// 把框架打成单文件 ESM(out/momo_framework.js), 供 snapshot / 调试用。
import path from "node:path";
import * as esbuild from "esbuild";
import { baseOptions, frameworkRoot, momoExternal, srcRoot } from "./common.mjs";

await esbuild.build({
  ...baseOptions,
  entryPoints: [path.join(srcRoot, "index.ts")],
  outfile: path.join(frameworkRoot, "out/momo_framework.js"),
  sourcemap: false,
  plugins: [momoExternal],
});
console.log("built out/momo_framework.js");
