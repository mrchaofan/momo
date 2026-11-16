#!/usr/bin/env node
// 构建运行时自带的 JS 模块(engine/common/settings.h 的 builtin_dir;CMake target momo_builtin 调用):
//   node scripts/build_builtin.mjs <out-dir> <module>...
// 每个模块编译成 <out-dir>/<name>/{package.json, index.js}, 应用以 `import … from "momo:<name>"` 使用。
// 模块之间的引用(webview → momo:framework)和引擎的原生模块(momo:ui、momo:services)都是 momo:*, 原样保留,
// 运行时解析到同一份。内置扩展的原生插件(.node)由 CMake 直接输出到 <out-dir>/<name>/, 源码里按相对路径
// import 它, 这条导入也原样保留。
import fs from "node:fs";
import path from "node:path";
import * as esbuild from "esbuild";
import { baseOptions, frameworkRoot, srcRoot, webviewRoot } from "./common.mjs";

const entries = {
  framework: path.join(srcRoot, "index.ts"),
  webview: path.join(webviewRoot, "js/index.ts"),
};

const [outDir, ...names] = process.argv.slice(2);
if (!outDir || names.length === 0 || names.some((name) => !(name in entries))) {
  console.error(`usage: build_builtin.mjs <out-dir> <${Object.keys(entries).join("|")}>...`);
  process.exit(64);
}
const { version } = JSON.parse(fs.readFileSync(path.join(frameworkRoot, "packages/momo/package.json"), "utf8"));

await Promise.all(names.map(async (name) => {
  const dir = path.join(path.resolve(outDir), name);
  await esbuild.build({
    ...baseOptions,
    external: [...baseOptions.external, "*.node"],
    entryPoints: [entries[name]],
    outfile: path.join(dir, "index.js"),
  });
  fs.writeFileSync(path.join(dir, "package.json"),
                   `${JSON.stringify({ name: `momo-${name}`, version, type: "module", main: "index.js" }, null, 2)}\n`);
}));
