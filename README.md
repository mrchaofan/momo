# Momo — 桌面端自绘跨端框架

Momo 是一个面向 macOS 与 Windows 的**自绘**（self-rendered）桌面应用框架。你用 TypeScript/JavaScript 编写界面和业务逻辑，产物是一个原生应用（macOS 上是 `.app`）。所有像素都由 Skia 绘制，不依赖系统控件，因此双端外观一致。

分层借鉴 Flutter（Framework / Engine / Embedder），但框架层用 TypeScript 而不是 Dart：

```
应用代码（TS）  ──  runApp(new MaterialApp({ home: ... }))
Framework（TS） ──  View / Element / RenderObject、布局、手势、动画、Material 控件；app / Widget（应用与窗口）；由运行时自带（momo:framework）
Engine（C++）   ──  V8 · momo:ui 绑定 · DisplayList / LayerTree · Skia（Metal / 软件）· 文本 · 扩展宿主能力表
Embedder（C++） ──  进程入口 · 消息循环 · 窗口 · 渲染表面 · VSync · 输入 / IME
```

> 想了解内部是怎么实现的，读 [ARCHITECTURE.md](ARCHITECTURE.md)；接手维护先读 [ONBOARDING.md](ONBOARDING.md)。本文只讲怎么用。

## 目录

- [快速开始](#快速开始)
- [编写第一个应用](#编写第一个应用)
- [两种写法：View 框架与 momo:ui 底层 API](#两种写法view-框架与-momoui-底层-api)
- [常用能力速查](#常用能力速查)
- [调试](#调试)
- [打包发布](#打包发布)
- [原生扩展](#原生扩展)
- [命令行参数与环境变量](#命令行参数与环境变量)
- [示例](#示例)
- [仓库结构与文档](#仓库结构与文档)
- [当前状态](#当前状态)

---

## 快速开始

### 环境要求（macOS）

| 工具 | 版本 |
|---|---|
| macOS | Apple Silicon（arm64）；V8 预编译包要求 macOS ≥ 13 |
| Xcode Command Line Tools | 提供 clang（C++20）与 Metal / AppKit SDK |
| CMake + Ninja | CMake ≥ 3.24 |
| Python 3 | 运行依赖下载脚本 |
| Node.js + npm | 构建运行时自带的框架与内置扩展（esbuild）、编译示例应用、运行框架测试 |

### 1. 拉取预编译依赖

Skia 与 V8 使用预编译包（版本与 SHA256 锁定在 `cmake/deps.lock.json`），下载到 `third_party/prebuilt/`：

```bash
python3 tools/fetch_deps.py
```

### 2. 安装框架的构建依赖

框架（`momo:framework`）与内置扩展是运行时的一部分，构建运行时的时候用 esbuild 编译，所以要先装好它（没装时 CMake 只给出警告，构建出的运行时不带 `momo:framework`）：

```bash
(cd framework && npm install)
```

### 3. 构建运行时 Momo.app

```bash
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

```bash
cmake --build out/debug
```

产物 `out/debug/embedder/Momo.app` 是**通用运行时**：它负责创建窗口、启动引擎并执行一个应用，自带框架与内置扩展（`Contents/Resources/builtin/`）。默认打包进去的应用是 `examples/hello_frame`，直接打开就能看到一个动画画面：

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo
```

### 4. 运行一个 View 示例

示例应用不依赖任何 npm 包，不需要 `npm install`；`npm run build` 把 `src/main.ts` 编译成 `dist/main.js`（借用上面装在 `framework/` 下的 esbuild）：

```bash
(cd examples/counter && npm run build)
```

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev examples/counter
```

---

## 编写第一个应用

一个 Momo 应用就是一个目录：`package.json` 的 `main` 是运行时加载的入口（JS，ESM），资源放在 `assets/`：

```
my-app/
├── package.json
├── src/
│   └── main.ts
└── assets/            # 可选：图片等资源，运行时通过 AssetImage("logo.png") 等访问
```

框架不在应用里：它由运行时提供，应用写 `import … from "momo:framework"` 就能用，框架的版本就是运行时的版本。内置扩展也一样（目前只有 macOS 上的 `momo:webview`）。所以应用的文件里只有你自己的代码，不需要为了框架安装任何 npm 包。

`package.json`（`momo.id` 是应用标识，打包时必填；`momo.name` 是打包后的 `.app` 名称）：

```json
{
  "name": "my-app",
  "private": true,
  "type": "module",
  "main": "dist/main.js",
  "momo": { "id": "com.example.myapp", "name": "MyApp" },
  "scripts": {
    "build": "esbuild src/main.ts --bundle --format=esm --platform=neutral '--external:momo:*' --sourcemap=inline --outfile=dist/main.js"
  }
}
```

源码怎么变成 `main` 是应用自己的事，Momo 不参与，用什么工具都可以（纯 JS 应用不需要编译，`main` 直接指向源码）。要求只有两条：产物是 ESM；`momo:*` 的导入原样保留（esbuild 是 `--external:momo:*`）。上面用的是 esbuild，在 `my-app` 里装一次：`npm install --save-dev esbuild`。

`src/main.ts`：

```ts
import {
  Center, Column, ElevatedButton, MaterialApp, Scaffold, AppBar, SizedBox,
  State, StatefulView, Text, runApp, type BuildContext, type View,
} from "momo:framework";

class CounterPage extends StatefulView {
  createState(): State {
    return new CounterPageState();
  }
}

class CounterPageState extends State<CounterPage> {
  #count = 0;

  build(context: BuildContext): View {
    return new Scaffold({
      appBar: new AppBar({ title: new Text("我的第一个 Momo 应用") }),
      body: new Center({
        child: new Column({
          mainAxisSize: "min",
          children: [
            new Text(`点击了 ${this.#count} 次`),
            new SizedBox({ height: 16 }),
            new ElevatedButton({
              onPressed: () => this.setState(() => this.#count++),
              child: new Text("+1"),
            }),
          ],
        }),
      }),
    });
  }
}

runApp(new MaterialApp({ title: "MyApp", home: new CounterPage() }));
```

### 开发循环：编译 + 运行

用应用自己的构建脚本把源码编译到 `main`，再用 `--dev` 启动运行时，参数是应用目录：

```bash
(cd my-app && npm run build)
```

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev my-app
```

重新编译后重启运行时即可看到改动。Momo 不支持热重启：一个进程里只有一个 JS 运行时，换应用就是换进程。

### 类型检查与编辑器提示

`momo:framework` 的实现在运行时里，类型来自 npm 包 `@momo/framework`（以 TS 源码发布，目前通过本地路径引用：`npm install --save-dev ../momo-djs/framework/packages/momo`）。应用只用它做类型检查，在 `tsconfig.json` 里把 `momo:framework` 指过去：

```json
{
  "compilerOptions": {
    "module": "ESNext",
    "moduleResolution": "bundler",
    "noEmit": true,
    "paths": { "momo:framework": ["./node_modules/@momo/framework/src/index.ts"] }
  }
}
```

`momo:webview` 目前没有单独发布的类型包（仓库内的示例靠 `framework/tsconfig.json` 的 `paths` 指到 `extensions/webview/js/index.ts`）。

### 写给 Flutter 开发者

API 形状与 Flutter 基本一致，主要差异：

- 构造参数用一个**选项对象**：`new Padding({ padding: EdgeInsets.all(8), child })`，而不是命名参数。
- 枚举多用字符串字面量：`mainAxisSize: "min"`、`crossAxisAlignment: "start"`。
- 颜色是 `new Color(0xff4285f4)` 或 `Colors.*`。
- 没有 `runtimeType`：两个 View 可以复用同一 Element 的条件是**构造函数相同且 key 相等**。
- 运行环境里没有 DOM API（没有 `fetch`）；UI 里的延迟任务请用 `SchedulerBinding.instance.scheduleDelayed(ms, fn)`（由定时器驱动，等待期间不出帧；测试里走假时钟），网络、文件等能力由扩展提供。

---

## 两种写法：View 框架与 momo:ui 底层 API

绝大多数应用用 View 框架（上一节）。如果你要写游戏、可视化，或者想了解引擎边界，也可以绕开框架、直接用引擎提供的 `momo:ui` 模块画帧。它对标 Flutter 的 `dart:ui`，是框架与引擎之间的内部接口（框架随运行时一起发布，这个接口跟着运行时的版本变），完整类型声明见 [framework/packages/momo/src/ui/momo-ui.d.ts](framework/packages/momo/src/ui/momo-ui.d.ts)。

`momo:ui` 只有引擎职责：`platformDispatcher` 是全局的帧调度与帧回调，`viewports` 是视图（窗口）的画面、尺寸与输入。应用生命周期与窗口的标题、位置、标题栏等属于宿主，经宿主注册的 `momo/app`、`momo/window` 通道操作（框架里是 `app` 与 `Widget` 对象，以及 `WindowChrome` / `Title`）。

```js
import { window, views, PictureRecorder, Paint, SceneBuilder } from "momo:ui";

const view = views.get(0);  // 宿主启动时创建的窗口（隐式视图）

window.onBeginFrame = (timeMs) => {
  const dpr = view.devicePixelRatio;
  const w = view.physicalSize.width / dpr;            // 以逻辑像素绘制
  const h = view.physicalSize.height / dpr;

  const recorder = new PictureRecorder();
  const canvas = recorder.beginRecording(0, 0, w, h);  // Canvas 只记录指令（DisplayList），不立即绘制
  canvas.drawColor(0xfff5f5f7);
  const paint = new Paint();
  paint.color = 0xff4285f4;
  const x = (Math.sin(timeMs / 500) * 0.5 + 0.5) * (w - 100);
  canvas.drawRect(x, h / 2 - 50, 100, 100, paint);

  const scene = new SceneBuilder()
    .pushTransform([dpr, 0, 0, 0, 0, dpr, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])  // 逻辑像素 → 物理像素
    .addPicture(0, 0, recorder.endRecording())
    .pop()
    .build();
  view.render(scene);         // 提交本视图本帧的画面
  window.scheduleFrame();     // 请求下一帧（不请求就不会出帧，空闲零功耗）
};
view.onMetricsChanged = () => window.scheduleFrame();
window.scheduleFrame();
```

这类脚本不需要构建，直接运行：`Momo --dev path/to/main.js`（应用也可以是单个 JS 文件）。更完整的例子（含经 `momo/window` 通道设置窗口标题）见 `examples/hello_frame`。

---

## 常用能力速查

以下 API 都从 `momo:framework` 导入。

| 需求 | 用法 |
|---|---|
| 布局 | `Row` / `Column` / `Flex` / `Expanded` / `Stack` / `Positioned` / `Padding` / `Align` / `Center` / `SizedBox` / `Container` |
| 滚动列表 | `ListView`（含 `ListView.builder` 懒构建）、`SingleChildScrollView`、`ScrollController` |
| 文本 | `Text`、`RichText`、`TextStyle`、`DefaultTextStyle` |
| 输入框 | `TextField` + `TextEditingController`（支持系统输入法） |
| 点击 / 拖拽 | `GestureDetector({ onTap, onDoubleTap, onPan..., child })`、`Listener`、`MouseRegion`（hover 与光标） |
| 键盘快捷键 | `Shortcuts` + `Actions` + `SingleActivator(LogicalKeyboardKey.xxx)`；焦点用 `Focus` / `FocusNode` |
| 图片 | `Image.asset("logo.png")`（读 `assets/` 目录）、`new Image(new MemoryImage(bytes))` |
| 动画 | `AnimationController`、`Tween` / `CurvedAnimation`、`AnimatedBuilder`；隐式动画 `AnimatedContainer` / `AnimatedOpacity` |
| Material 控件 | `MaterialApp`、`Scaffold`、`AppBar`、`ElevatedButton` / `TextButton` / `FloatingActionButton`、`Checkbox`、`ListTile`、`Card`、`showDialog` / `AlertDialog`、`Theme` / `ThemeData` |
| 自定义绘制 | `CustomPaint`（拿到 `momo:ui` 的 `Canvas`） |
| 剪贴板 | `await Clipboard.setData("text")`、`await Clipboard.getData()` |
| 打开链接 | `await openUrl("https://example.com")` |
| 应用 | `app.quit()`（可被 `before-quit` / 窗口 `close` / `will-quit` 取消）、`app.exit(code)`（不询问）、`app.hide()` / `app.focus()`；事件 `app.on("window-all-closed" / "activate" / "state-changed", ...)`。**最后一个窗口关闭后应用不会自动退出**，单窗口应用写 `app.on("window-all-closed", () => app.quit())` |
| 拦截关闭 | 关窗：`widget.on("close", (e) => e.preventDefault())`；退出应用（Cmd+Q）：`app.on("before-quit", (e) => e.preventDefault())`。需要异步确认时用 `e.waitUntil(promise)`（例如提示"有未保存内容"） |
| 窗口 | `await Widget.create({ title, width, height, show, ... })` 打开窗口（`Widget.fromId(0)` 是启动窗口），内容用 `ViewAnchor({ views: [new RootView({ widget, child })] })` 渲染；`Title` / `MaterialApp({ title })` 设置所在窗口的标题；窗口方法 `close` / `destroy` / `show` / `hide` / `minimize` / `maximize` / `restore` / `focus` / `setPosition`，事件 `closed` / `focus` / `minimize` / `move` / `resize` / `ready-to-show`… |
| 菜单 | `Menu.buildFromTemplate([{ role: "appMenu" }, { label: "File", submenu: [{ label: "New", accelerator: "CmdOrCtrl+N", click }] }, { role: "editMenu" }])`；`Menu.setApplicationMenu(menu)` 设为应用菜单（macOS 菜单栏、Windows 每个窗口的菜单栏）；`await menu.popup({ widget, x, y })` 弹出上下文菜单。菜单项的 `label` / `enabled` / `visible` / `checked` 可随时修改；`role` 是由系统执行的标准动作（退出、复制、最小化…） |
| 托盘 | `const tray = new Tray("trayTemplate.png")`（相对应用的 `assets/`；macOS 用 PNG，文件名以 `Template` 结尾为模板图像；Windows 用 `.ico`）；`tray.setToolTip(...)`、`tray.setContextMenu(menu)`、`tray.on("click", ...)`、`tray.destroy()` |
| 无边框窗口 | `WindowChrome({ titleBarHeight, resizeBorder })` 隐藏系统标题栏；系统窗口控制按钮占用的区域见 `MediaQuery.of(ctx).windowControls`，两侧留白用 `windowControlsPadding`（macOS 在左、Windows 在右，一份代码适配）；`WindowDragArea` 声明可拖动窗口的区域 |
| 窗口能力 | `widget.capabilities`、`widget.bounds`、`widget.state`（平台不支持的能力是空操作，可先查询） |
| 网页 | 内置扩展 `momo:webview`（macOS）：`import { WebView } from "momo:webview"`（见 [extensions/webview/README.md](extensions/webview/README.md)） |
| 调用原生代码 | 原生扩展是 Node.js 原生插件，以普通 npm 包的形式使用：`import { add } from "@acme/ext-foo"`（见下文） |

完整用法可参考 `examples/counter`（按钮、主题、快捷键）、`examples/todo`（输入框、列表、勾选、删除、滚动），以及 `framework/test/` 下的测试用例。

---

## 调试

- **Chrome DevTools**：`--dev` 启动时引擎在 `127.0.0.1:9229` 开启 Node 的 inspector，启动日志（stderr）会打印 `Debugger listening on ws://…`；也可以在 `chrome://inspect` 中添加 `localhost:9229`。支持断点、单步、console、性能分析、堆快照。`--inspect-brk` 让引擎在执行 bundle 前等待调试器连接；`--inspect-port=<n>` 换端口。
- **Node.js API**：JS 运行在嵌入的 Node.js 26 环境中，可以直接 `import` `node:fs`、`node:net`、`node:child_process` 等内置模块和 node_modules 里的 npm 包，`setTimeout`、`process.nextTick` 按 Node 语义工作（docs/04 §1）。
- **日志**：`console.log/warn/error` 写入引擎日志，从终端启动 `Momo.app/Contents/MacOS/Momo` 即可看到；连上 DevTools 时也会出现在 DevTools 控制台（含调用栈）。
- **异常**：build 中抛出的异常会让对应子树显示为 `ErrorView`；布局、指针/键盘事件处理中的异常会被报告并跳过。都不会让应用崩溃。
- **软件渲染**：`MOMO_SOFTWARE_RENDERING=1` 强制使用 CPU 光栅（排查 GPU 相关问题）。没有可用的 Metal 设备时也会自动回退到软件渲染。

---

## 打包发布

命令行工具是 npm 包 `@momo/cli`（源码在 `framework/packages/cli`，命令名 `momo`），随每个版本发布（`momo-cli-v<version>.tgz`）。它不依赖源码树：装进自己的工程后即可使用，仓库内则直接用 `node framework/packages/cli/momo.mjs` 运行。

CLI 只有一个命令。`momo build` 把应用打包成可分发的 `.app`（可选 DMG）：复制运行时 `Momo.app` 为 `<Name>.app` → 把应用目录里选中的文件原样、保持相对位置放进 `Contents/Resources/app/`。它不编译（打包前先用应用自己的构建脚本产出 `main`），不分析文件内容，也不知道依赖。

带哪些文件只按文件名决定：`package.json`、`main`、`assets/**` 总是带上，`momo.include` 追加任意其他文件，`momo.ignore` 从全部结果里去掉（通配：`*`、`**`、`?`，以 `/` 结尾表示整个目录）。应用运行时要从 `node_modules` 加载的包（没有编译进 `main` 的，例如带原生插件的包，见下文“使用三方扩展”）用 `momo.include` 带上，如 `"include": ["node_modules/better-sqlite3/"]`；它自己还依赖别的包时，把那些包也写上。没匹配到任何文件的 include（写错了或依赖没安装）会报错，`main` 不在结果里（还没编译，或被 ignore 去掉）也会报错。

运行时不随 CLI 分发，用 `--runtime=` 或环境变量 `MOMO_RUNTIME` 指定：可以是发布产物 `momo-v<version>-<platform>.zip` 解压出的 `Momo.app`，也可以是自己构建的。

```bash
node framework/packages/cli/momo.mjs build my-app --out=out/apps --runtime=out/debug/embedder/Momo.app
```

```bash
node framework/packages/cli/momo.mjs build my-app --out=out/apps --runtime=out/debug/embedder/Momo.app --dmg
```

| 选项 | 说明 |
|---|---|
| `--out=<dir>` | 输出目录（默认 `<app-dir>/dist`） |
| `--name=<App>` | 应用名（默认取 `package.json` 的 `momo.name`，其次是 `name`） |
| `--runtime=<Momo.app>` | 使用的运行时（必填，或设环境变量 `MOMO_RUNTIME`；发布时请用 Release 构建的运行时） |
| `--dmg` | 额外用 `hdiutil` 产出 `<Name>.dmg` |

打包后的应用资源布局：

```
MyApp.app/Contents/
├── MacOS/Momo                   # 运行时可执行文件
├── Frameworks/libnode.*.dylib     # Node.js(含 V8)
└── Resources/
    ├── builtin/                 # 运行时自带的模块（随运行时复制过来）
    │   ├── framework/           # momo:framework：package.json、index.js
    │   └── webview/             # momo:webview：package.json、index.js、ext_webview.node
    └── app/                     # 你的应用，与 my-app 里的相对位置一致
        ├── package.json
        ├── dist/main.js         # package.json 的 main：只有你自己的代码
        ├── assets/              # 来自 my-app/assets
        └── node_modules/…       # momo.include 带上的包（如带原生插件的三方包）
```

非 `--dev` 模式下，引擎会把 JS 编译结果缓存在 `~/Library/Caches/<bundle identifier>/v8`（每个应用一个目录；默认的 Momo.app 是 `dev.momo.app`），加快之后的启动。

---

## 原生扩展

框架内置的 API 不够用时（数据库、音视频、硬件、现有 C/C++ SDK……），可以写**原生扩展**。Momo 的 JS 运行在 Node.js 环境里，所以原生扩展就是 **Node.js 原生插件**（`.node`，Node-API）：一个普通的 npm 包 = JS 门面 + 一个 `.node` 文件，npm 上现成的原生包按设计也用同一条路加载（没有拿真实的 npm 原生包测过）。JS 与原生之间只用 Node-API；需要引擎能力（平台线程、外部纹理、平台视图）的扩展另外使用一张很小的宿主能力表 [momo_host.h](extensions/sdk/include/momo/momo_host.h)（稳定的纯 C ABI，只在末尾追加）。

扩展有两种来源：

- **内置扩展**随运行时发布，和框架一样以 `momo:<name>` 导入，应用什么都不用装。目前只有 `momo:webview`（macOS，源码在 `extensions/webview/`）。
- **三方扩展**是应用自己的依赖，属于业务代码：Momo 没有为它准备任何配置项，下面两小节讲的就是它。

### 扩展 SDK（构建产物）

SDK 是编译一个能被 Momo 加载的插件所需的全部。构建 Momo 时，SDK 会放到 `out/<build>/sdk/`，也可以安装到任意目录；发布产物是 `momo-ext-sdk-v<version>-<platform>.zip`：

```bash
cmake --install out/debug --prefix ~/momo-sdk --component sdk
```

```
sdk/
├── include/momo/momo_host.h          # 宿主能力表（稳定 C ABI）
├── include/node/                     # Momo 运行时所带 Node 的头文件（node_api.h 等；不含 openssl）
├── Release/node.lib                  # 仅 Windows：libnode.dll 的导入库
└── lib/cmake/MomoExtSdk/             # find_package(MomoExtSdk) 的包配置
```

扩展工程的 CMake 只需要两行（`CMakeLists.txt` 放在 `<包>/native/`），`momo_add_extension()` 产出 `<包>/build/<name>.node`：

```cmake
find_package(MomoExtSdk 1 REQUIRED)        # -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk
momo_add_extension(ext_acme src/ext_acme.cc)   # → <包>/build/ext_acme.node
```

### 使用三方扩展

三方扩展对应用来说就是一个普通 npm 依赖，由 Node 按自己的规则从应用的 `node_modules` 加载，开发时和打包后都一样。没有 manifest，也没有宿主侧的扩展列表。

带原生插件的包必须**保持原样留在 `node_modules` 里**：包内的 JS 按相对路径找自己的 `.node` 与动态库，编译进 `main` 就找不到了。所以应用要做两件事，都在自己的 `package.json` 里（以 `extensions/examples/hello-extension` 为例，先 `npm install ../momo-djs/extensions/examples/hello-extension`）：

```json
"scripts": {
  "build": "esbuild src/main.ts --bundle --format=esm --platform=neutral '--external:momo:*' --external:@momo-examples/ext-hello --sourcemap=inline --outfile=dist/main.js"
},
"momo": {
  "include": ["node_modules/@momo-examples/ext-hello/"]
}
```

- 构建配置里把这个包排除（esbuild 是 `--external:<包名>`），导入在 `main` 里原样保留；
- 用 `momo.include` 把 `node_modules/<包>/` 带进应用包（只有打包时需要；`Momo --dev my-app` 直接用应用目录里的 `node_modules`）。

```ts
import { add, greet, hash } from "@momo-examples/ext-hello";
console.log(add(1, 2), greet("Momo"), await hash(new Uint8Array([1, 2, 3])));
```

不带原生插件的纯 JS 包两种方式都可以：编译进 `main`，或者同样留在 `node_modules` 里。扩展需要框架时（带视图的扩展）直接 `import "momo:framework"`：它由运行时提供，与应用拿到的是同一份，所以这样的扩展也是一个保持原样的整包。

### 编写三方扩展

扩展是一个普通的 npm 包，结构参照 `extensions/examples/hello-extension`：

```
ext-myext/
├── package.json          # type: module，main: js/index.js，types: js/index.d.ts
├── js/index.js           # JS 门面：按相对路径 import 插件
├── js/index.d.ts         # 门面的类型
├── native/
│   ├── CMakeLists.txt    # 上面那两行
│   └── src/myext.c       # Node-API 模块
└── build/myext.node      # 编译产物
```

原生侧是标准的 Node-API 模块：

```c
#include <node_api.h>

static napi_value Add(napi_env env, napi_callback_info info) {     // JS（ui）线程调用，禁止阻塞
  size_t argc = 2;
  napi_value argv[2], result;
  double a, b;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
  if (argc < 2 || napi_get_value_double(env, argv[0], &a) != napi_ok ||
      napi_get_value_double(env, argv[1], &b) != napi_ok) {
    napi_throw_type_error(env, NULL, "add(a, b) expects two numbers");
    return NULL;
  }
  napi_create_double(env, a + b, &result);
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  napi_create_function(env, "add", NAPI_AUTO_LENGTH, Add, NULL, &fn);
  napi_set_named_property(env, exports, "add", fn);
  return exports;
}
```

JS 门面按相对路径 import 这个 `.node`（默认导出是插件的 exports）。它是 JS 而不是 TS：这个包保持原样由 Node 加载，运行时不执行 TS；类型写在旁边的 `.d.ts` 里：

```js
// js/index.js
import addon from "../build/myext.node";
export const { add } = addon;
```

```ts
// js/index.d.ts
export function add(a: number, b: number): number;
```

Promise、后台线程（`napi_async_work`）、从别的线程回到 JS（`napi_threadsafe_function`）都用 Node-API。插件内部可以使用异常，但异常不得穿过 Node-API 与宿主表的回调边界。

需要引擎能力时，JS 门面把 `momo:services` 的 `nativeHost`（一个 BigInt）传给插件，插件用 `momo_host_from_napi(env, value)` 取出宿主能力表。表里只有三样：把任务投到 platform（OS 主）线程（`post_platform_task`，原生 UI 只能在那里用）、外部纹理（扩展自己生成像素，在界面中用 `Texture` 视图显示）、平台视图（把系统原生视图嵌进界面，如 WebView）。

**回调线程规则**：宿主只在 platform 线程调用扩展，而且只有两类回调——平台任务与视图工厂。引擎关闭时先关闭宿主表、销毁全部平台视图，再销毁 JS 运行时；扩展用 `napi_add_env_cleanup_hook` 登记的清理函数在最后这一步执行，在那里停掉自己的线程。

示例：

| 示例 | 演示 |
|---|---|
| `extensions/examples/hello-extension` | 纯 C，只用 Node-API：同步函数、字符串/字节、线程池上的 Promise |
| `extensions/examples/texture-extension` | C++：外部纹理，自己的线程按约 60 fps 推帧 |
| `extensions/webview` | 内置扩展 `momo:webview`，ObjC++（macOS）：WKWebView 平台视图；带视图的扩展，JS（`js/index.ts`）与原生插件在运行时的同一个目录里；示例应用 `examples/webview` |

设计细节见 [docs/07-extension-system.md](docs/07-extension-system.md)。

---

## 命令行参数与环境变量

运行时 `Momo [<app>] [--key=value ...]` 的参数（未知参数与 `-psn_*`、`-NS*` 等系统参数会被忽略）：

| 参数 | 说明 |
|---|---|
| `<app>` | 应用路径（第一个位置参数）：目录（入口是其 `package.json` 的 `main`，缺省 `index.js`）或一个 JS 文件；资源在它的 `assets/`。默认是应用包内的 `<Resources>/app` |
| `--title=<text>` | 初始窗口标题 |
| `--width=<px>` / `--height=<px>` | 初始窗口尺寸（逻辑像素） |
| `--dev` | 开发模式：启用 Inspector |
| `--inspect-port=<n>` | Inspector 端口（默认 9229，`-1` 关闭） |
| `--inspect-brk` | 执行 bundle 前等待调试器连接（等待期间窗口照常响应；此时退出应用，要等调试器连上并断开后才完成） |

| 环境变量 | 说明 |
|---|---|
| `MOMO_SOFTWARE_RENDERING=1` | 强制使用 CPU 软件渲染 |
| `MOMO_UPDATE_GOLDENS=1` | 运行测试时重新生成 golden 基准图 |

---

## 示例

| 示例 | 层次 | 内容 |
|---|---|---|
| `examples/hello_frame` | momo:ui | 渐变卡片、旋转星形、描边；默认打包进 Momo.app |
| `examples/counter` | View | Material 计数器：按钮、主题、键盘快捷键 |
| `examples/todo` | View | 输入框、添加/勾选/删除、列表滚动 |
| `examples/webview` | View + `momo:webview` | 多标签浏览器（macOS）：内置扩展 webview、无边框窗口、标签拖拽 |

View 示例不依赖任何 npm 包，不需要 `npm install`：在示例目录里 `npm run build`（借用 `framework/node_modules` 里的 esbuild）编译后以 `Momo --dev <示例目录>` 运行，也可以再用 `momo build`（示例里是 `npm run package`，需设 `MOMO_RUNTIME`）打包成 `.app`。

---

## 仓库结构与文档

| 目录 | 内容 |
|---|---|
| `framework/` | TypeScript UI 框架（运行时以 `momo:framework` 提供；npm 包 `@momo/framework` 供类型检查）及其测试、命令行工具 `@momo/cli` |
| `engine/` | 核心引擎（C++）：V8 运行时、`momo:*` 绑定、渲染管线、文本、资源、扩展宿主能力表；`engine/platform/` 是宿主需要实现的平台契约 |
| `embedder/` | 宿主层：进程入口、生命周期、Engine 装配；`common/` 平台无关，`macos/` / `windows/` / `headless/` 为各平台实现 |
| `base/` | C++ 基础库：Object 模型、EventLoop、TaskRunner、Status |
| `extensions/` | 扩展 SDK（宿主能力表头文件 + CMake 包配置）、内置扩展（`webview/`）与三方扩展的示例（`examples/`） |
| `examples/` | 示例应用 |
| `tools/` | `fetch_deps.py`（依赖下载）、`package_release.py`（发布产物） |
| `cmake/`、`third_party/` | CMake 模块与第三方依赖 |
| `docs/` | 设计文档 |

- [ARCHITECTURE.md](ARCHITECTURE.md)：模块划分与运行机制（embedder / engine / framework 如何实现，含时序图与伪代码）。
- [ONBOARDING.md](ONBOARDING.md)：给新维护者：第一天跑通、阅读路线、代码地图、必须遵守的规则、常见改动步骤、测试与调试。
- `docs/`：设计文档，从 [docs/00-overview.md](docs/00-overview.md) 读起；里程碑与逐项状态见 [docs/11-roadmap.md](docs/11-roadmap.md)。

### 从源码构建与测试

```bash
cmake --build out/debug && (cd out/debug && ctest)
```

```bash
cd framework && npm test
```

C++ 测试经 CTest 运行（gtest + golden 像素比对）；框架测试同时在 Node（mock `momo:ui`）与真实引擎 headless 宿主（`momo_headless`）下运行。

---

## 当前状态

- M0–M5 已实现，并在 macOS 上构建与测试：CTest 215 项全部通过（其中 9 项因沙箱环境没有 GPU、显示器或端口绑定而跳过）；框架 TS 测试在 Node mock 与真实引擎 headless 两种宿主下均通过。
- M6 完成了 V8 code cache、精简版 Material、`.app` / DMG 打包；多窗口、无障碍、崩溃上报、性能浮层未做。
- **Windows**：平台层（与 macOS 同一套指针派发、窗口能力、应用生命周期、菜单与托盘）与 D3D12 GPU 后端已实现，但**未在 Windows 上编译运行验证**（macOS 上只做了单测与交叉编译检查；菜单与托盘 `win_menu.cc` 连交叉编译检查也没有做）；不支持平台视图。
- 技术选型：C++20（禁用异常与 RTTI）· V8 · Skia（macOS 走 Metal，CPU 光栅为回退）· SkParagraph（含 ICU）· CMake + Ninja。

逐项状态见 [docs/11-roadmap.md](docs/11-roadmap.md)。
