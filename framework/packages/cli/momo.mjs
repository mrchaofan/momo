#!/usr/bin/env node
// momo — Momo 应用/扩展开发者命令行(npm 包 @momo/cli;docs/07 §2、docs/09 §4-6)。
//
//   momo bundle <app-dir> [--out=<dir>] [--minify]
//       只出应用 bundle, 不装配运行时(开发循环用: Momo --dev --bundle=<out>/bundle.js):
//       esbuild 打应用 bundle → <out>/bundle.js(momo:* 保持外部导入;默认带内联 sourcemap, --minify 时压缩
//       且不带)。代码里 import 的原生扩展(`import addon from "../build/x.node"`, docs/07)拷到 <out>/native/,
//       bundle 从那里加载。<out> 默认为 <app-dir>/dist。
//   momo build <app-dir> [--out=<dir>] [--runtime=<Momo.app|dir>] [--name=<App>] [--minify] [--dmg]
//       装配可分发的应用: 运行时 + `momo bundle` 的产物 + assets/。
//       运行时是发布产物 momo-v<version>-<platform>.zip 解压后的内容(或自己构建的), 由 --runtime 或环境变量
//       MOMO_RUNTIME 给出: macOS 为 Momo.app, Windows 为含 Momo.exe 与 libnode.dll 的目录。
//       macOS: 复制 Momo.app → <out>/<Name>.app, 放入 Resources/{bundle.js, native/, assets/};
//       Windows: → <out>/<Name>/{<Name>.exe, *.dll, bundle.js, native/, assets/}(exe 同级即资源目录,
//       docs/09 §4);其他平台: 输出目录布局 <out>/<name>/{bundle.js, native/, assets/}
//       --dmg(macOS): 再用 hdiutil 产出 <Name>.dmg
//   momo ext create <dir> [--name=<thing>]   原生扩展脚手架(Node 插件 + JS 门面)
//
// 应用目录约定: package.json(main = 入口 .ts/.js;dependencies 里有 @momo/framework 与用到的扩展包(普通 npm 包);
// momo.name 应用名, momo.icon 应用图标 .icns(macOS)), 可选 assets/ 目录。
// 依赖怎么找: 装在 node_modules 里的包按 Node 的规则解析;以 file: 引用的本地包直接按它的 package.json 解析,
// 不要求先 npm install(仓库内的示例与本地开发的扩展就是这样引用的)。
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { baseOptions, momoExternal } from "./bundle_options.mjs";

function fail(message) {
  console.error(`momo: ${message}`);
  process.exit(1);
}

function parseArgs(argv) {
  const positional = [];
  const flags = {};
  for (const arg of argv) {
    if (arg.startsWith("--")) {
      const eq = arg.indexOf("=");
      if (eq < 0) flags[arg.slice(2)] = true;
      else flags[arg.slice(2, eq)] = arg.slice(eq + 1);
    } else {
      positional.push(arg);
    }
  }
  return { positional, flags };
}

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch (e) {
    fail(`cannot read ${file}: ${e.message}`);
  }
}

// ---------------------------------------------------------------------------
// 依赖

/**
 * 依赖包在哪: {dir, local}。指向目录的 file: 引用是本地包(local), 直接用那个目录;其余(版本号、指向 tarball 的
 * file: …)是装好的包, 从声明它的包向上找 node_modules/<name>。找不到为 null。
 */
function resolveDependency(fromDir, name, spec) {
  if (typeof spec === "string" && spec.startsWith("file:")) {
    const target = path.resolve(fromDir, spec.slice(5));
    if (fs.statSync(target, { throwIfNoEntry: false })?.isDirectory()) return { dir: target, local: true };
  }
  let dir = fromDir;
  while (true) {
    const candidate = path.join(dir, "node_modules", name);
    if (fs.existsSync(path.join(candidate, "package.json"))) return { dir: candidate, local: false };
    const parent = path.dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

/**
 * 应用的全部依赖(含传递依赖): [{name, dir, pkg, local}], 供打 bundle 时解析本地包。
 */
function collectDependencies(appDir) {
  const seen = new Set();
  const out = [];
  const visit = (name, spec, fromDir) => {
    const found = resolveDependency(fromDir, name, spec);
    if (found === null || seen.has(found.dir)) return;
    seen.add(found.dir);
    const pkg = readJson(path.join(found.dir, "package.json"));
    out.push({ name, pkg, ...found });
    for (const [n, s] of Object.entries(pkg.dependencies || {})) visit(n, s, found.dir);
  };
  const app = readJson(path.join(appDir, "package.json"));
  for (const [name, spec] of Object.entries(app.dependencies || {})) visit(name, spec, appDir);
  return out;
}

/** 包 dep 的导入路径 subpath("." 或 "./views")对应的文件: 按它的 package.json(exports, 否则 main)。 */
function packageEntry(dep, subpath) {
  const exports = dep.pkg.exports;
  if (typeof exports === "string") return subpath === "." ? path.resolve(dep.dir, exports) : null;
  if (exports && typeof exports === "object") {
    return typeof exports[subpath] === "string" ? path.resolve(dep.dir, exports[subpath]) : null;
  }
  return subpath === "." ? path.resolve(dep.dir, dep.pkg.main || "index.js") : path.resolve(dep.dir, subpath);
}

/** esbuild 插件: 本地(file:)包按包名解析到它的入口——等同于 npm install 之后的结果, 但不要求先安装。 */
function localPackagesPlugin(dependencies) {
  return {
    name: "momo-local-packages",
    setup(build) {
      for (const dep of dependencies.filter((d) => d.local)) {
        const escaped = dep.name.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&");
        build.onResolve({ filter: new RegExp(`^${escaped}(/.*)?$`) }, (args) => {
          const subpath = `.${args.path.slice(dep.name.length)}`;
          const file = packageEntry(dep, subpath);
          return file !== null ? { path: file }
            : { errors: [{ text: `package ${dep.name} (${dep.dir}) does not export '${subpath}'` }] };
        });
      }
    },
  };
}

/**
 * esbuild 插件: 原生扩展(docs/07)。按路径 import 的 .node 文件拷到 <outDir>/native/, bundle 里的导入改为指向
 * 那里(运行时的模块钩子负责加载 .node)。.node 是编译产物: 不存在说明扩展还没构建。
 */
function nativeAddonsPlugin(outDir) {
  const nativeDir = path.join(outDir, "native");
  const copied = new Map();  // 文件名 → 源路径
  return {
    name: "momo-native-addons",
    setup(build) {
      build.onStart(() => {
        fs.rmSync(nativeDir, { recursive: true, force: true });
        copied.clear();
      });
      build.onResolve({ filter: /^(\.{1,2}[\\/]|\/|[A-Za-z]:[\\/]).*\.node$/ }, (args) => {
        const source = path.resolve(args.resolveDir, args.path);
        const name = path.basename(source);
        if (!fs.existsSync(source)) {
          return { errors: [{ text: `native addon ${source} does not exist (build the extension first)` }] };
        }
        if (copied.has(name) && copied.get(name) !== source) {
          return { errors: [{ text: `two native addons are both named ${name}: ${copied.get(name)} and ${source}` }] };
        }
        if (!copied.has(name)) {
          fs.mkdirSync(nativeDir, { recursive: true });
          fs.copyFileSync(source, path.join(nativeDir, name));
          copied.set(name, source);
          console.log(`native addon: ${name}`);
        }
        return { path: `./native/${name}`, external: true };
      });
    },
  };
}

// ---------------------------------------------------------------------------
// bundle

/** 应用的 JS 部分 → <outDir>/bundle.js 与(用到原生扩展时)<outDir>/native/*.node。 */
async function writeBundle(appDir, outDir, { minify }) {
  const pkg = readJson(path.join(appDir, "package.json"));
  const entry = path.resolve(appDir, pkg.main || "src/main.ts");
  if (!fs.existsSync(entry)) fail(`entry not found: ${entry}`);
  const dependencies = collectDependencies(appDir);
  let esbuild;
  try {
    esbuild = await import("esbuild");
  } catch {
    fail("esbuild is not installed (it is a dependency of @momo/cli: run `npm install`)");
  }
  fs.mkdirSync(outDir, { recursive: true });
  await esbuild.build({
    ...baseOptions,
    entryPoints: [entry],
    outfile: path.join(outDir, "bundle.js"),
    minify: !!minify,
    sourcemap: minify ? false : "inline",
    plugins: [momoExternal, localPackagesPlugin(dependencies), nativeAddonsPlugin(outDir)],
  });
}

async function bundle({ positional, flags }) {
  const appDir = path.resolve(positional[0] || ".");
  const outDir = path.resolve(flags.out || path.join(appDir, "dist"));
  await writeBundle(appDir, outDir, { minify: flags.minify });
  console.log(`built ${path.relative(process.cwd(), path.join(outDir, "bundle.js"))}`);
}

// ---------------------------------------------------------------------------
// build

function copyDir(from, to) {
  if (!fs.existsSync(from)) return;
  fs.cpSync(from, to, { recursive: true });
}

async function build({ positional, flags }) {
  const appDir = path.resolve(positional[0] || ".");
  const pkg = readJson(path.join(appDir, "package.json"));
  const appName = flags.name || (pkg.momo && pkg.momo.name) || pkg.name.replace(/^@.*\//, "");
  const outDir = path.resolve(flags.out || path.join(appDir, "dist"));

  fs.mkdirSync(outDir, { recursive: true });
  let resources;
  let bundleRoot;
  // 运行时不随 CLI 分发: 由调用方给出(发布产物 momo-v<version>-<platform>.zip 解压后的内容, 或自己的构建)。
  const runtimeFlag = flags.runtime || process.env.MOMO_RUNTIME;
  const runtimeOf = (what) => {
    if (!runtimeFlag) fail(`no Momo runtime: pass --runtime=<${what}> or set MOMO_RUNTIME (the unpacked momo-v<version>-<platform>.zip release asset, or your own build)`);
    return path.resolve(runtimeFlag);
  };
  if (process.platform === "darwin") {
    const runtime = runtimeOf("Momo.app");
    if (!fs.existsSync(path.join(runtime, "Contents/MacOS"))) fail(`not a Momo.app runtime: ${runtime}`);
    bundleRoot = path.join(outDir, `${appName}.app`);
    fs.rmSync(bundleRoot, { recursive: true, force: true });
    fs.cpSync(runtime, bundleRoot, { recursive: true, verbatimSymlinks: true });
    resources = path.join(bundleRoot, "Contents/Resources");
    // 替换 Info.plist 中的显示名(可执行文件名保持 Momo)。
    const plist = path.join(bundleRoot, "Contents/Info.plist");
    // 应用图标: package.json "momo.icon" 指向 .icns, 拷为 Resources/AppIcon.icns 并写入 CFBundleIconFile。
    const iconSrc = pkg.momo && pkg.momo.icon ? path.resolve(appDir, pkg.momo.icon) : null;
    if (iconSrc !== null) {
      if (!fs.existsSync(iconSrc) || !iconSrc.endsWith(".icns")) fail(`momo.icon must point to an existing .icns file: ${iconSrc}`);
      fs.mkdirSync(resources, { recursive: true });
      fs.copyFileSync(iconSrc, path.join(resources, "AppIcon.icns"));
    }
    if (fs.existsSync(plist)) {
      let text = fs.readFileSync(plist, "utf8").replace(/(<key>CFBundleName<\/key>\s*<string>)[^<]*/, `$1${appName}`);
      if (iconSrc !== null) {
        if (/<key>CFBundleIconFile<\/key>/.test(text)) {
          text = text.replace(/(<key>CFBundleIconFile<\/key>\s*<string>)[^<]*/, "$1AppIcon");
        } else {
          text = text.replace(/<dict>/, "<dict>\n\t<key>CFBundleIconFile</key>\n\t<string>AppIcon</string>");
        }
      }
      fs.writeFileSync(plist, text);
    }
  } else if (process.platform === "win32") {
    const runtime = runtimeOf("directory with Momo.exe");
    const exe = path.join(runtime, "Momo.exe");
    if (!fs.existsSync(exe)) fail(`not a Momo runtime directory (no Momo.exe): ${runtime}`);
    bundleRoot = path.join(outDir, appName);
    fs.rmSync(bundleRoot, { recursive: true, force: true });
    fs.mkdirSync(bundleRoot, { recursive: true });
    fs.copyFileSync(exe, path.join(bundleRoot, `${appName}.exe`));
    for (const entry of fs.readdirSync(runtime)) {
      if (entry.toLowerCase().endsWith(".dll") || entry === "icudtl.dat") {
        fs.copyFileSync(path.join(runtime, entry), path.join(bundleRoot, entry));
      }
    }
    resources = bundleRoot;
  } else {
    bundleRoot = path.join(outDir, appName);
    resources = bundleRoot;
  }
  fs.mkdirSync(resources, { recursive: true });
  await writeBundle(appDir, resources, { minify: flags.minify });
  copyDir(path.join(appDir, "assets"), path.join(resources, "assets"));
  if (process.platform === "darwin") {
    // 修改了包内容: 重新 ad-hoc 签名(否则 Gatekeeper/代码签名校验失败)。
    try {
      execFileSync("codesign", ["--force", "--deep", "--sign", "-", bundleRoot], { stdio: "ignore" });
    } catch {
      console.warn("  ! codesign failed (the app still runs from the build machine)");
    }
  }
  console.log(`built ${path.relative(process.cwd(), bundleRoot)}`);
  if (flags.dmg && process.platform === "darwin") {
    const dmg = path.join(outDir, `${appName}.dmg`);
    fs.rmSync(dmg, { force: true });
    execFileSync("hdiutil", ["create", "-volname", appName, "-srcfolder", bundleRoot, "-ov", "-format", "UDZO", dmg], { stdio: "inherit" });
    console.log(`built ${path.relative(process.cwd(), dmg)}`);
  }
}

// ---------------------------------------------------------------------------
// ext create

function create({ positional, flags }) {
  const dir = path.resolve(positional[0] || "");
  if (!positional[0]) fail("usage: momo ext create <dir> [--name=<thing>]");
  const name = flags.name || path.basename(dir).replace(/^ext[-_]/, "");
  if (!/^[a-z][a-z0-9_]*$/.test(name)) fail(`extension name must be lowercase letters, digits and _ (got '${name}'); pass --name=`);
  if (fs.existsSync(dir) && fs.readdirSync(dir).length) fail(`${dir} is not empty`);
  const files = {
    "package.json": JSON.stringify({ name: `ext-${name}`, version: "0.1.0", main: "js/index.ts" }, null, 2) + "\n",
    "js/index.ts": `// JS 门面: 给原生插件的函数加上类型。插件由 native/ 构建到 build/${name}.node。
import addon from "../build/${name}.node";

const native = addon as { hello(who: string): string };

export function hello(who: string): string {
  return native.hello(who);
}
`,
    "native/CMakeLists.txt": `cmake_minimum_required(VERSION 3.24)
project(${name} LANGUAGES C CXX)
# 扩展 SDK(发布产物 momo-ext-sdk-v<version>-<platform>.zip 解压后的目录, 或 Momo 构建目录下的 sdk/):
#   cmake -S . -B out -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk && cmake --build out
find_package(MomoExtSdk 1 REQUIRED)
# 产物: ../build/${name}.node。
momo_add_extension(${name} src/${name}.c)
`,
    [`native/src/${name}.c`]: `/* 一个 Node 插件(Node-API)。需要平台线程、外部纹理或平台视图时, 另外 #include "momo/momo_host.h":
 * JS 侧把 momo:services 的 nativeHost 传进来, 这里用 momo_host_from_napi 取出宿主表。 */
#include <stdio.h>

#include <node_api.h>

static napi_value Hello(napi_env env, napi_callback_info info) {
  size_t argc = 1, len = 0;
  napi_value argv[1], result;
  char who[256];
  char out[300];
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 1 || napi_get_value_string_utf8(env, argv[0], who, sizeof(who), &len) != napi_ok) {
    napi_throw_type_error(env, NULL, "hello(who) expects a string");
    return NULL;
  }
  snprintf(out, sizeof(out), "hello, %s", who);
  napi_create_string_utf8(env, out, NAPI_AUTO_LENGTH, &result);
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  napi_create_function(env, "hello", NAPI_AUTO_LENGTH, Hello, NULL, &fn);
  napi_set_named_property(env, exports, "hello", fn);
  return exports;
}
`,
  };
  for (const [rel, content] of Object.entries(files)) {
    const file = path.join(dir, rel);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content);
  }
  const native = path.join(dir, "native");
  console.log(`created extension ${name} in ${path.relative(process.cwd(), dir) || "."}`);
  console.log(`  build: cmake -S ${native} -B ${path.join(native, "out")} -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk && cmake --build ${path.join(native, "out")}`);
}

// ---------------------------------------------------------------------------

const [command, ...rest] = process.argv.slice(2);
if (command === "bundle") {
  await bundle(parseArgs(rest));
} else if (command === "build") {
  await build(parseArgs(rest));
} else if (command === "ext" && rest[0] === "create") {
  create(parseArgs(rest.slice(1)));
} else {
  // 文件头注释(第 2 行起, 到第一个非注释行为止)即用法说明。
  const header = fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").slice(1);
  console.log(header.slice(0, header.findIndex((l) => !l.startsWith("//"))).map((l) => l.replace(/^\/\/ ?/, "")).join("\n"));
  process.exit(command ? 1 : 0);
}
