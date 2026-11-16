#!/usr/bin/env node
// momo — 把一个 Momo 应用打包成可分发的应用(npm 包 @momo/cli;docs/09 §4)。
//
//   momo build <app-dir> [--out=<dir>] [--runtime=<Momo.app|dir>] [--name=<App>] [--dmg]
//       复制运行时 → 把应用目录里选中的文件原样放进 <resources>/app/。运行时是发布产物
//       momo-v<version>-<platform>.zip 解压后的内容(或自己构建的), 由 --runtime 或环境变量 MOMO_RUNTIME 给出:
//       macOS 为 Momo.app, Windows 为含 Momo.exe 的目录。
//       macOS: <out>/<Name>.app(应用在 Contents/Resources/app/);Windows: <out>/<Name>/{<Name>.exe, *.dll, builtin/, app/};
//       其他平台没有运行时, 只输出 <out>/<Name>/app/。<out> 默认为 <app-dir>/dist。
//       --dmg(macOS): 再用 hdiutil 产出 <Name>.dmg
//
// 应用 = 一个目录, 约定都在它的 package.json 里:
//   main            运行时加载的入口(JS)。`Momo <app-dir>` 与打包后的应用都从这里启动(只有这一项是运行时读的)
//   momo.include    带进应用包的文件(通配, 相对应用目录)。package.json、main、assets/** 总是带上
//   momo.ignore     从全部结果里去掉的文件(通配)
//   momo.name       应用名(缺省取 name, --name 优先);momo.icon 应用图标 .icns(macOS)
// 通配: * 匹配一段路径里的任意字符, ** 匹配任意层目录, ? 匹配一个字符;以 / 结尾表示整个目录。
//
// 本工具只按文件名挑选、原样复制、保持相对位置。它不编译, 不分析文件内容, 也不知道依赖:
//   - 框架与内置扩展由运行时提供(import "momo:framework"、"momo:webview"…), 不在应用的文件里。
//   - 应用的源码怎么变成 main(TS 编译、打不打成一个文件)用应用自己选的工具;唯一的要求是 momo:* 的导入原样
//     保留(esbuild: --external:momo:*), 产物是 ESM。
//   - 应用用到的三方包(包括带原生插件 .node 的)是应用自己的事: 要么编译进 main, 要么留在 node_modules 里由
//     Node 加载——留下的就用 momo.include 带上, 例如 "node_modules/better-sqlite3/"。
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

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

// ---------------------------------------------------------------------------
// FileSet: 按通配挑选一个目录下的文件

/** 通配 → 匹配整条相对路径(用 / 分隔)的正则。 */
function globToRegExp(glob) {
  let re = "";
  for (let i = 0; i < glob.length; i++) {
    const c = glob[i];
    if (c === "*" && glob[i + 1] === "*") {
      i++;
      if (glob[i + 1] === "/") {
        i++;
        re += "(?:.*/)?";  // "**/" 也匹配零层目录
      } else {
        re += ".*";
      }
    } else if (c === "*") {
      re += "[^/]*";
    } else if (c === "?") {
      re += "[^/]";
    } else {
      re += c.replace(/[.+^${}()|[\]\\]/g, "\\$&");
    }
  }
  return new RegExp(`^${re}$`);
}

class Pattern {
  constructor(glob) {
    this.glob = glob;
    const normalized = path.posix.normalize(glob.replace(/\\/g, "/"));
    const full = normalized.endsWith("/") ? `${normalized}**` : normalized;
    this.regexp = globToRegExp(full);
    // 逐段的匹配: 用来判断一个目录下面还可能不可能有匹配的文件(不可能就不必进入)。
    this.segments = full.split("/").map((segment) => (segment.includes("**") ? null : globToRegExp(segment)));
  }
  matches(file) {
    return this.regexp.test(file);
  }
  /** 目录 dir(相对路径)下面可能有匹配的文件。 */
  reaches(dir) {
    const names = dir.split("/");
    for (let i = 0; i < names.length; i++) {
      if (i >= this.segments.length - 1 && this.segments[i] !== null) return false;  // 最后一段是文件名
      if (this.segments[i] === null) return true;  // ** 之下都有可能
      if (!this.segments[i].test(names[i])) return false;
    }
    return true;
  }
}

/** 一个目录下被 include 选中、又没被 ignore 去掉的文件。 */
class FileSet {
  constructor(root, include, ignore) {
    this.root = root;
    this.include = include.map((glob) => new Pattern(glob));
    this.ignore = ignore.map((glob) => new Pattern(glob));
  }

  /** 选中的文件(相对路径, / 分隔, 已排序)。跟随符号链接(node_modules 里链接进来的本地包)。 */
  list() {
    const files = [];
    const walk = (dir, ancestors) => {
      const real = fs.realpathSync(path.join(this.root, dir));
      if (ancestors.includes(real)) return;  // 链接成环
      for (const name of fs.readdirSync(real).sort()) {
        const rel = dir === "" ? name : `${dir}/${name}`;
        const stat = fs.statSync(path.join(real, name), { throwIfNoEntry: false });  // 跟随链接;断链为空
        if (stat?.isDirectory()) {
          if (this.include.some((p) => p.reaches(rel))) walk(rel, [...ancestors, real]);
        } else if (stat?.isFile() && this.include.some((p) => p.matches(rel)) &&
                   !this.ignore.some((p) => p.matches(rel))) {
          files.push(rel);
        }
      }
    };
    walk("", []);
    return files;
  }

  /** 把选中的文件原样复制到 to(保持相对位置;链接复制为它指向的文件)。 */
  copyTo(to) {
    for (const file of this.list()) {
      const target = path.join(to, file);
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.copyFileSync(path.join(this.root, file), target);
    }
  }
}

// ---------------------------------------------------------------------------
// App: 应用目录。package.json 里的约定只在这里解释

class App {
  constructor(dir, { name } = {}) {
    this.dir = path.resolve(dir);
    const file = path.join(this.dir, "package.json");
    let pkg;
    try {
      pkg = JSON.parse(fs.readFileSync(file, "utf8"));
    } catch (e) {
      fail(`not a Momo app: cannot read ${file}: ${e.message}`);
    }
    const momo = pkg.momo ?? {};
    this.name = name ?? momo.name ?? String(pkg.name ?? path.basename(this.dir)).replace(/^@.*\//, "");
    this.main = pkg.main ?? "index.js";  // 运行时的缺省值(engine/common/app_layout.h)
    this.icon = momo.icon ? path.resolve(this.dir, momo.icon) : null;
    this.include = momo.include ?? [];
    this.ignore = momo.ignore ?? [];
    for (const key of ["include", "ignore"]) {
      if (!Array.isArray(this[key]) || this[key].some((item) => typeof item !== "string" || item === "")) {
        fail(`package.json momo.${key} must be an array of glob patterns`);
      }
    }
  }

  /** 应用包里的文件: package.json、main、assets/**, 加上 momo.include, 去掉 momo.ignore。 */
  files() {
    const always = ["package.json", this.main, "assets/**"];
    const set = new FileSet(this.dir, [...always, ...this.include], this.ignore);
    const files = set.list();
    if (!files.some((file) => set.include[1].matches(file))) {
      fail(`the entry ${this.main} (package.json main) is not among the packaged files: it does not exist ` +
           "(build the app first?) or momo.ignore removes it");
    }
    // 没带上任何文件的 include 是写错了(或依赖没安装): 不要等到运行时才发现。
    for (const pattern of set.include.slice(always.length)) {
      if (!files.some((file) => pattern.matches(file))) fail(`momo.include "${pattern.glob}" matches no files`);
    }
    return set;
  }
}

// ---------------------------------------------------------------------------
// AppShell: 把运行时复制成一个应用外壳。平台差异只在这里;应用文件怎么放进去与平台无关

class AppShell {
  /** 当前平台的外壳。运行时不随 CLI 分发, 由调用方给出。 */
  static create(app, outDir, runtime) {
    const Shell = process.platform === "darwin" ? MacAppShell : process.platform === "win32" ? WindowsAppShell : AppShell;
    if (Shell.runtimeKind !== null && !runtime) {
      fail(`no Momo runtime: pass --runtime=<${Shell.runtimeKind}> or set MOMO_RUNTIME (the unpacked ` +
           "momo-v<version>-<platform>.zip release asset, or your own build)");
    }
    return new Shell(app, outDir, runtime ? path.resolve(runtime) : null);
  }

  static runtimeKind = null;  // 没有运行时的平台: 只输出应用目录

  constructor(app, outDir, runtime) {
    this.app = app;
    this.runtime = runtime;
    this.root = path.join(outDir, app.name);
  }
  /** <resources>: 运行时在这里找 app/(engine/common/app_layout.h)。 */
  get resources() {
    return this.root;
  }
  /** 复制运行时, 得到一个还没有应用的外壳。 */
  copyRuntime() {
    fs.mkdirSync(this.root, { recursive: true });
  }
  /** 应用文件放进去之后的收尾(签名等)。 */
  seal() {}

  /** 外壳 + 应用文件 → 可分发的应用。返回其路径。 */
  assemble(files) {
    fs.rmSync(this.root, { recursive: true, force: true });
    this.copyRuntime();
    const appDir = path.join(this.resources, "app");
    fs.rmSync(appDir, { recursive: true, force: true });  // 运行时自带的默认应用
    files.copyTo(appDir);
    this.seal();
    return this.root;
  }
}

class MacAppShell extends AppShell {
  static runtimeKind = "Momo.app";

  constructor(app, outDir, runtime) {
    super(app, outDir, runtime);
    this.root = path.join(outDir, `${app.name}.app`);
  }
  get resources() {
    return path.join(this.root, "Contents/Resources");
  }
  copyRuntime() {
    if (!fs.existsSync(path.join(this.runtime, "Contents/MacOS"))) fail(`not a Momo.app runtime: ${this.runtime}`);
    const { icon, name } = this.app;
    if (icon !== null && !(fs.existsSync(icon) && icon.endsWith(".icns"))) {
      fail(`momo.icon must point to an existing .icns file: ${icon}`);
    }
    fs.cpSync(this.runtime, this.root, { recursive: true, verbatimSymlinks: true });
    // Info.plist: 显示名(可执行文件名保持 Momo)与图标(Resources/AppIcon.icns)。
    if (icon !== null) fs.copyFileSync(icon, path.join(this.resources, "AppIcon.icns"));
    const plist = path.join(this.root, "Contents/Info.plist");
    let text = fs.readFileSync(plist, "utf8").replace(/(<key>CFBundleName<\/key>\s*<string>)[^<]*/, `$1${name}`);
    if (icon !== null) {
      text = /<key>CFBundleIconFile<\/key>/.test(text)
        ? text.replace(/(<key>CFBundleIconFile<\/key>\s*<string>)[^<]*/, "$1AppIcon")
        : text.replace(/<dict>/, "<dict>\n\t<key>CFBundleIconFile</key>\n\t<string>AppIcon</string>");
    }
    fs.writeFileSync(plist, text);
  }
  seal() {
    // 修改了包内容: 重新 ad-hoc 签名(否则 Gatekeeper/代码签名校验失败)。
    try {
      execFileSync("codesign", ["--force", "--deep", "--sign", "-", this.root], { stdio: "ignore" });
    } catch {
      console.warn("  ! codesign failed (the app still runs from the build machine)");
    }
  }
  /** <Name>.dmg。 */
  writeDmg() {
    const dmg = this.root.replace(/\.app$/, ".dmg");
    fs.rmSync(dmg, { force: true });
    execFileSync("hdiutil", ["create", "-volname", this.app.name, "-srcfolder", this.root, "-ov", "-format", "UDZO", dmg],
                 { stdio: "inherit" });
    return dmg;
  }
}

class WindowsAppShell extends AppShell {
  static runtimeKind = "directory with Momo.exe";

  copyRuntime() {
    const exe = path.join(this.runtime, "Momo.exe");
    if (!fs.existsSync(exe)) fail(`not a Momo runtime directory (no Momo.exe): ${this.runtime}`);
    fs.mkdirSync(this.root, { recursive: true });
    fs.copyFileSync(exe, path.join(this.root, `${this.app.name}.exe`));
    for (const entry of fs.readdirSync(this.runtime)) {
      if (entry.toLowerCase().endsWith(".dll") || entry === "icudtl.dat") {
        fs.copyFileSync(path.join(this.runtime, entry), path.join(this.root, entry));
      }
    }
    // 运行时自带的 JS 模块(框架、内置扩展)。
    fs.cpSync(path.join(this.runtime, "builtin"), path.join(this.root, "builtin"), { recursive: true });
  }
}

// ---------------------------------------------------------------------------
// 命令

const shown = (file) => path.relative(process.cwd(), file) || ".";

function build({ positional, flags }) {
  const app = new App(positional[0] || ".", { name: flags.name });
  const shell = AppShell.create(app, path.resolve(flags.out || path.join(app.dir, "dist")),
                                flags.runtime || process.env.MOMO_RUNTIME);
  if (flags.dmg && !(shell instanceof MacAppShell)) fail("--dmg is only available on macOS");
  console.log(`built ${shown(shell.assemble(app.files()))}`);
  if (flags.dmg) console.log(`built ${shown(shell.writeDmg())}`);
}

// ---------------------------------------------------------------------------

const [command, ...rest] = process.argv.slice(2);
if (command === "build") {
  build(parseArgs(rest));
} else {
  // 文件头注释(第 2 行起, 到第一个非注释行为止)即用法说明。
  const header = fs.readFileSync(fileURLToPath(import.meta.url), "utf8").split("\n").slice(1);
  console.log(header.slice(0, header.findIndex((l) => !l.startsWith("//"))).map((l) => l.replace(/^\/\/ ?/, "")).join("\n"));
  process.exit(command ? 1 : 0);
}
