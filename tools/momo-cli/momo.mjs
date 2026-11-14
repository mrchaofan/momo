#!/usr/bin/env node
// momo — Momo 应用/扩展开发者命令行(docs/07 §7、docs/09 §4-6)。
//
//   momo build <app-dir> [--out=<dir>] [--runtime=<Momo.app>] [--native-dir=<dir>] [--name=<App>]
//              [--minify] [--dmg]
//       1) esbuild 打应用 bundle(momo:* / ext:* 保持外部导入;@momo/framework 解析到框架源码)
//       2) 扫描依赖中的 package.json "momo.extension" 声明 → 生成 extensions.manifest,
//          拷贝当前平台的原生库;打印每个扩展申请的 capabilities(构建期可见性, docs/07 §6)
//       3) macOS: 复制预编译运行时 Momo.app → <out>/<Name>.app, 放入 Resources/{bundle.js, assets/,
//          extensions/};Windows: 运行时目录(--runtime, 含 Momo.exe 与 libnode.dll)→ <out>/<Name>/
//          {<Name>.exe, *.dll, bundle.js, assets/, extensions/}(exe 同级即资源目录, docs/09 §4);
//          其他平台: 输出目录布局 <out>/<name>/{bundle.js, assets/, extensions/}
//       --dmg(macOS): 再用 hdiutil 产出 <Name>.dmg
//   momo ext create <dir> --id=<acme.thing> [--name=<thing>]   扩展脚手架(C ABI 模板 + JS 门面)
//   momo ext verify <library> [--package=<package.json>]        ABI 符号检查 + manifest 声明校验
//   momo manifest <app-dir> [--native-dir=<dir>]                只打印将生成的 extensions.manifest
//
// 应用目录约定: package.json(main = 入口 .ts/.js;dependencies 可用 file: 引用本地扩展包;
// momo.name 应用名, momo.icon 应用图标 .icns(macOS)), 可选 assets/ 目录。
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const frameworkScripts = path.join(repoRoot, "framework/scripts/common.mjs");

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

export function platformKey() {
  const p = process.platform === "darwin" ? "macos" : process.platform === "win32" ? "windows" : process.platform;
  const a = process.arch === "arm64" ? "arm64" : process.arch === "x64" ? "x64" : process.arch;
  return `${p}-${a}`;
}

// ---------------------------------------------------------------------------
// 扩展发现

/** 解析依赖包目录: file: 引用 → 相对 app;否则 node_modules/<name>。 */
function resolveDependency(appDir, name, spec) {
  if (typeof spec === "string" && spec.startsWith("file:")) {
    return path.resolve(appDir, spec.slice(5));
  }
  let dir = appDir;
  while (true) {
    const candidate = path.join(dir, "node_modules", name);
    if (fs.existsSync(path.join(candidate, "package.json"))) return candidate;
    const parent = path.dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

function findFile(root, basename) {
  if (!root || !fs.existsSync(root)) return null;
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const full = path.join(root, entry.name);
    if (entry.isFile() && entry.name === basename) return full;
    if (entry.isDirectory() && !entry.name.startsWith(".") && entry.name !== "node_modules" &&
        !entry.name.endsWith(".app") && !entry.name.endsWith(".dSYM")) {
      const found = findFile(full, basename);
      if (found) return found;
    }
  }
  return null;
}

/** 返回 [{id, specifier, capabilities, packageDir, packageName, library(源路径), entry(JS 门面)}]。 */
export function collectExtensions(appDir, nativeDir) {
  const pkg = readJson(path.join(appDir, "package.json"));
  const deps = { ...(pkg.dependencies || {}) };
  const seen = new Set();
  const out = [];
  const key = platformKey();
  const visit = (name, spec, fromDir) => {
    const dir = resolveDependency(fromDir, name, spec);
    if (!dir || seen.has(dir)) return;
    seen.add(dir);
    const depPkg = readJson(path.join(dir, "package.json"));
    const ext = depPkg.momo && depPkg.momo.extension;
    if (ext) {
      if (!/^[a-z0-9]+(\.[a-z0-9_-]+)+$/i.test(ext.id || "")) fail(`${name}: momo.extension.id must be reverse-DNS (got '${ext.id}')`);
      if (ext.specifier && !/^ext:[\w-]+$/.test(ext.specifier)) fail(`${name}: specifier must be 'ext:<name>'`);
      const declared = ext.native && ext.native[key];
      let library = declared ? path.resolve(dir, declared) : null;
      if (library && !fs.existsSync(library)) {
        // 仓库内开发: prebuilt 尚不存在时到构建目录中按文件名查找。
        library = findFile(nativeDir, path.basename(declared));
      }
      out.push({
        id: ext.id,
        specifier: ext.specifier || null,
        capabilities: ext.capabilities || [],
        packageDir: dir,
        packageName: depPkg.name,
        declared: declared || null,
        library,
        entry: path.resolve(dir, depPkg.main || "index.js"),
      });
    }
    for (const [n, s] of Object.entries(depPkg.dependencies || {})) visit(n, s, dir);
  };
  for (const [name, spec] of Object.entries(deps)) visit(name, spec, appDir);
  return out;
}

export function manifestFor(extensions) {
  return {
    extensions: extensions.filter((e) => e.library).map((e) => ({
      id: e.id,
      library: path.basename(e.library),
      specifier: e.specifier,
      capabilities: e.capabilities,
    })),
  };
}

// ---------------------------------------------------------------------------
// build

async function bundleApp(appDir, outFile, extensions, minify) {
  const pkg = readJson(path.join(appDir, "package.json"));
  const entry = path.resolve(appDir, pkg.main || "src/main.ts");
  if (!fs.existsSync(entry)) fail(`entry not found: ${entry}`);
  const { baseOptions, momoExternal, momoFrameworkAlias } = await import(pathToFileURL(frameworkScripts).href);
  const esbuildPath = path.join(repoRoot, "framework/node_modules/esbuild/lib/main.js");
  if (!fs.existsSync(esbuildPath)) fail("esbuild not installed: run `npm install` in framework/");
  const esbuild = await import(pathToFileURL(esbuildPath).href);
  // 本地(file:)扩展包: 包名 → JS 门面入口。
  const localPackages = {
    name: "momo-local-packages",
    setup(build) {
      for (const ext of extensions) {
        const escaped = ext.packageName.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&");
        build.onResolve({ filter: new RegExp(`^${escaped}$`) }, () => ({ path: ext.entry }));
      }
    },
  };
  await esbuild.build({
    ...baseOptions,
    entryPoints: [entry],
    outfile: outFile,
    minify: !!minify,
    sourcemap: minify ? false : "inline",
    plugins: [momoExternal, momoFrameworkAlias, localPackages],
  });
}

function copyDir(from, to) {
  if (!fs.existsSync(from)) return;
  fs.cpSync(from, to, { recursive: true });
}

async function build({ positional, flags }) {
  const appDir = path.resolve(positional[0] || ".");
  const pkg = readJson(path.join(appDir, "package.json"));
  const appName = flags.name || (pkg.momo && pkg.momo.name) || pkg.name.replace(/^@.*\//, "");
  const outDir = path.resolve(flags.out || path.join(appDir, "dist"));
  const nativeDir = flags["native-dir"] ? path.resolve(flags["native-dir"]) : path.join(repoRoot, "out/debug/extensions");
  const extensions = collectExtensions(appDir, nativeDir);

  fs.mkdirSync(outDir, { recursive: true });
  let resources;
  let bundleRoot;
  if (process.platform === "darwin") {
    const runtime = path.resolve(flags.runtime || path.join(repoRoot, "out/debug/embedder/Momo.app"));
    if (!fs.existsSync(runtime)) fail(`runtime not found: ${runtime} (build momo_app_mac first or pass --runtime=)`);
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
    const runtime = path.resolve(flags.runtime || path.join(repoRoot, "out/debug/embedder"));
    const exe = path.join(runtime, "Momo.exe");
    if (!fs.existsSync(exe)) fail(`runtime not found: ${exe} (build momo_app_win first or pass --runtime=<dir>)`);
    bundleRoot = path.join(outDir, appName);
    fs.rmSync(bundleRoot, { recursive: true, force: true });
    fs.mkdirSync(bundleRoot, { recursive: true });
    fs.copyFileSync(exe, path.join(bundleRoot, `${appName}.exe`));
    for (const entry of fs.readdirSync(runtime)) {
      if (entry.toLowerCase().endsWith(".dll")) fs.copyFileSync(path.join(runtime, entry), path.join(bundleRoot, entry));
    }
    resources = bundleRoot;
  } else {
    bundleRoot = path.join(outDir, appName);
    resources = bundleRoot;
  }
  fs.mkdirSync(resources, { recursive: true });
  await bundleApp(appDir, path.join(resources, "bundle.js"), extensions, flags.minify);
  copyDir(path.join(appDir, "assets"), path.join(resources, "assets"));

  const extDir = path.join(resources, "extensions");
  fs.rmSync(extDir, { recursive: true, force: true });
  if (extensions.length > 0) {
    fs.mkdirSync(extDir, { recursive: true });
    console.log("extensions:");
    for (const ext of extensions) {
      if (!ext.library) {
        console.warn(`  ! ${ext.id}: no native library for ${platformKey()} (${ext.declared || "not declared"}) — JS only`);
        continue;
      }
      fs.copyFileSync(ext.library, path.join(extDir, path.basename(ext.library)));
      const caps = ext.capabilities.length ? ext.capabilities.join(", ") : "none";
      console.log(`  ${ext.id} (${ext.specifier ?? "no JS module"}) capabilities: ${caps}`);
    }
    fs.writeFileSync(path.join(extDir, "extensions.manifest"), JSON.stringify(manifestFor(extensions), null, 2));
  }
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
// ext verify / create

function exportedSymbols(library) {
  if (process.platform === "darwin" || process.platform === "linux") {
    const out = execFileSync("nm", ["-gU", library], { encoding: "utf8" });
    return out.split("\n").map((l) => l.trim().split(/\s+/).pop()).filter(Boolean)
      .map((s) => (process.platform === "darwin" ? s.replace(/^_/, "") : s));
  }
  const out = execFileSync("dumpbin", ["/exports", library], { encoding: "utf8" });
  return out.split("\n").map((l) => l.trim().split(/\s+/).pop()).filter(Boolean);
}

function verify({ positional, flags }) {
  const library = positional[0] && path.resolve(positional[0]);
  if (!library || !fs.existsSync(library)) fail("usage: momo ext verify <library> [--package=package.json]");
  const problems = [];
  const symbols = exportedSymbols(library);
  if (!symbols.includes("momo_ext_init")) problems.push("does not export the C symbol momo_ext_init");
  const cxx = symbols.filter((s) => s.startsWith("_Z") || s.startsWith("?"));
  if (cxx.length) problems.push(`exports ${cxx.length} C++ (mangled) symbols; only momo_ext_init should be exported (use hidden visibility)`);
  const engineDeps = (() => {
    if (process.platform !== "darwin") return [];
    const out = execFileSync("otool", ["-L", library], { encoding: "utf8" });
    return out.split("\n").filter((l) => /libv8|libnode|momo_engine|libskia/.test(l));
  })();
  if (engineDeps.length) problems.push(`links engine-internal libraries (must use the C ABI only): ${engineDeps.join(", ")}`);
  if (flags.package) {
    const pkg = readJson(path.resolve(flags.package));
    const ext = pkg.momo && pkg.momo.extension;
    if (!ext) problems.push("package.json has no momo.extension");
    else {
      if (!/^[a-z0-9]+(\.[a-z0-9_-]+)+$/i.test(ext.id || "")) problems.push("momo.extension.id must be reverse-DNS");
      if (ext.specifier && !/^ext:[\w-]+$/.test(ext.specifier)) problems.push("momo.extension.specifier must be 'ext:<name>'");
      const known = new Set(["filesystem", "network", "window", "texture", "platform-view", "camera", "microphone", "clipboard"]);
      for (const c of ext.capabilities || []) if (!known.has(c)) problems.push(`unknown capability '${c}'`);
    }
  }
  if (problems.length) {
    for (const p of problems) console.error(`  ✗ ${p}`);
    process.exit(1);
  }
  console.log(`✓ ${path.basename(library)}: exports momo_ext_init, C ABI only`);
}

function create({ positional, flags }) {
  const dir = path.resolve(positional[0] || "");
  const id = flags.id;
  if (!positional[0] || !id) fail("usage: momo ext create <dir> --id=<acme.thing> [--name=<thing>]");
  const name = flags.name || id.split(".").pop();
  if (fs.existsSync(dir) && fs.readdirSync(dir).length) fail(`${dir} is not empty`);
  const files = {
    "package.json": JSON.stringify({
      name: `ext-${name}`, version: "0.1.0", main: "js/index.ts",
      momo: { extension: { id, specifier: `ext:${name}`, native: {
        "macos-arm64": `prebuilt/macos-arm64/libext_${name}.dylib`,
        "macos-x64": `prebuilt/macos-x64/libext_${name}.dylib`,
        "windows-x64": `prebuilt/windows-x64/ext_${name}.dll` }, capabilities: [] } },
    }, null, 2) + "\n",
    "js/index.ts": `import { native } from "ext:${name}";\n\nconst fns = native as unknown as Record<string, (...a: unknown[]) => unknown>;\n\nexport function hello(who: string): string {\n  return fns.hello(who) as string;\n}\n`,
    "native/CMakeLists.txt": `cmake_minimum_required(VERSION 3.24)\nproject(ext_${name} LANGUAGES C CXX)\n# SDK 位置: 源码树 extensions/sdk, 或 Momo 构建目录下的 sdk/(cmake --install 后为 <prefix>)。\nset(MomoExtSdk_DIR "${path.join(repoRoot, "extensions/sdk/lib/cmake/MomoExtSdk")}" CACHE PATH "Momo extension SDK")\nfind_package(MomoExtSdk 1 REQUIRED)\n# C++ 扩展可改用 .cc 源文件并 #include "momo/momo_extension.hpp"(见 extensions/examples/texture-extension)。\nmomo_add_extension(ext_${name} src/ext_${name}.c)\n`,
    [`native/src/ext_${name}.c`]: `#include <stdio.h>\n#include <string.h>\n\n#include "momo/momo_extension.h"\n\nstatic const MomoHostApi* g_host;\n\nstatic void Hello(void* user_data, MomoCallInfoRef call) {\n  char who[256];\n  char out[300];\n  size_t len = 0;\n  (void)user_data;\n  if (g_host->call->arg_string_utf8(call, 0, who, sizeof(who), &len) != kMomoOk) {\n    g_host->call->throw_error(call, "hello(who) expects a short string");\n    return;\n  }\n  snprintf(out, sizeof(out), "hello, %s", who);\n  g_host->call->return_string_utf8(call, out, strlen(out));\n}\n\nstatic void Setup(void* user_data, MomoJsBuilderRef builder) {\n  (void)user_data;\n  g_host->builder_add_function(builder, "hello", Hello, NULL);\n}\n\nstatic void OnAttach(void* user_data, const MomoHostApi* host, MomoEngineRef engine) {\n  (void)user_data;\n  g_host = host;\n  host->register_module(engine, "ext:${name}", Setup, NULL);\n}\n\nstatic void OnDetach(void* user_data) { (void)user_data; }\n\nMOMO_EXT_EXPORT bool momo_ext_init(uint32_t host_api_version, MomoExtension* out) {\n  if (host_api_version < MOMO_EXT_API_VERSION) return false;\n  out->id = "${id}";\n  out->on_attach = OnAttach;\n  out->on_detach = OnDetach;\n  return true;\n}\n`,
  };
  for (const [rel, content] of Object.entries(files)) {
    const file = path.join(dir, rel);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, content);
  }
  console.log(`created extension ${id} in ${path.relative(process.cwd(), dir) || "."}`);
  console.log(`  build: cmake -S ${path.join(dir, "native")} -B ${path.join(dir, "native/build")} && cmake --build ${path.join(dir, "native/build")}`);
}

// ---------------------------------------------------------------------------

const [command, sub, ...rest] = process.argv.slice(2);
if (command === "build") {
  await build(parseArgs([sub, ...rest].filter((x) => x !== undefined)));
} else if (command === "manifest") {
  const args = parseArgs([sub, ...rest].filter((x) => x !== undefined));
  const appDir = path.resolve(args.positional[0] || ".");
  const nativeDir = args.flags["native-dir"] ? path.resolve(args.flags["native-dir"]) : path.join(repoRoot, "out/debug/extensions");
  console.log(JSON.stringify(manifestFor(collectExtensions(appDir, nativeDir)), null, 2));
} else if (command === "ext" && sub === "verify") {
  verify(parseArgs(rest));
} else if (command === "ext" && sub === "create") {
  create(parseArgs(rest));
} else {
  console.log(fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").slice(1, 22).map((l) => l.replace(/^\/\/ ?/, "")).join("\n"));
  process.exit(command ? 1 : 0);
}
void os;
