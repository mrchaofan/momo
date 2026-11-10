#!/usr/bin/env node
// 用法: node framework/scripts/build_example.mjs <entry.ts> <out.js> [--minify] [--no-sourcemap]
// 产物为单文件 ESM bundle, 只保留 `momo:*` / `ext:*` 外部导入(引擎以 synthetic module 提供)。
import path from "node:path";
import * as esbuild from "esbuild";
import { baseOptions, momoExternal, momoFrameworkAlias } from "./common.mjs";

const args = process.argv.slice(2);
const positional = args.filter((a) => !a.startsWith("--"));
if (positional.length < 2) {
  console.error("usage: build_example.mjs <entry.ts> <out.js> [--minify] [--no-sourcemap]");
  process.exit(2);
}
const [entry, out] = positional.map((p) => path.resolve(p));
const result = await esbuild.build({
  ...baseOptions,
  entryPoints: [entry],
  outfile: out,
  minify: args.includes("--minify"),
  sourcemap: args.includes("--no-sourcemap") ? false : "inline",
  plugins: [momoExternal, momoFrameworkAlias],
  metafile: true,
});
const bytes = Object.values(result.metafile.outputs).reduce((s, o) => s + o.bytes, 0);
console.log(`built ${path.relative(process.cwd(), out)} (${(bytes / 1024).toFixed(1)} KiB)`);
