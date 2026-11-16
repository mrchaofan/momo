# Momo — 桌面端自绘跨端框架

Momo 是一个面向 macOS 与 Windows 的**自绘**（self-rendered）桌面应用框架。你用 TypeScript/JavaScript 编写界面和业务逻辑，产物是一个原生应用（macOS 上是 `.app`）。所有像素都由 Skia 绘制，不依赖系统控件。JS 运行在嵌入的 Node.js 里，`node:*` 模块与 npm 包可以直接用。

分层借鉴 Flutter（Framework / Engine / Embedder），框架层用 TypeScript：

```
应用代码（TS）  ──  runApp(new MaterialApp({ home: ... }))
Framework（TS） ──  View / Element / RenderObject、布局、手势、动画、Material 控件；app / Widget（应用与窗口）；由运行时自带（momo:framework）
Engine（C++）   ──  Node.js(V8) · momo:ui 绑定 · DisplayList / LayerTree · Skia（Metal / 软件）· 文本 · 扩展宿主能力表
Embedder（C++） ──  进程入口 · 消息循环 · 窗口 · 渲染表面 · VSync · 输入 / IME
```

**当前状态**：在 macOS（Apple Silicon）上构建并测试；Windows 的代码写了但**从未在 Windows 上运行验证**。详见 [docs/11-roadmap.md](docs/11-roadmap.md)。

本文只讲怎么用。其他文档：

| 文档 | 内容 |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | 内部怎么实现、为什么这样设计 |
| [ONBOARDING.md](ONBOARDING.md) | 接手维护：规则、阅读路线、常见改动的步骤 |
| [docs/07-extension-system.md](docs/07-extension-system.md) | 原生扩展 |
| [docs/09-build-and-deps.md](docs/09-build-and-deps.md) | 构建、依赖、打包、测试、发布 |
| [extensions/webview/README.md](extensions/webview/README.md) | 内置扩展 `momo:webview` 的接口 |

---

## 快速开始（macOS）

需要：Apple Silicon、macOS ≥ 13.5、Xcode Command Line Tools、CMake ≥ 3.24 + Ninja、Python 3、Node.js + npm。

```bash
python3 tools/fetch_deps.py
```

下载 Skia 预编译包并从源码构建 libnode（首次约 20~40 分钟），放到 `third_party/prebuilt/`。

```bash
(cd framework && npm install)
```

框架（`momo:framework`）是运行时的一部分，构建运行时的时候用 esbuild 编译，所以要先装好它。

```bash
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

```bash
cmake --build out/debug
```

产物 `out/debug/embedder/Momo.app` 是**通用运行时**：创建窗口、启动引擎并执行一个应用，自带框架与内置扩展。默认带的应用是 `examples/hello_frame`：

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo
```

运行一个 View 示例（示例不需要 `npm install`，`npm run build` 把 `src/main.ts` 编译成 `dist/main.js`）：

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
├── src/main.ts
└── assets/            # 可选：图片等资源，Image.asset("logo.png") 读这里
```

框架不在应用里：它由运行时提供，应用写 `import … from "momo:framework"` 就能用，框架的版本就是运行时的版本。所以应用的文件里只有你自己的代码。

`package.json`（`momo.id` 是应用标识，打包时必填）：

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

源码怎么变成 `main` 是应用自己的事，用什么工具都可以（纯 JS 应用不需要编译）。要求只有两条：产物是 ESM；`momo:*` 的导入原样保留。上面用的是 esbuild（`npm install --save-dev esbuild`）。

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

编译后用 `--dev` 启动运行时，参数是应用目录；改了代码就重新编译并重启（没有热重启）：

```bash
(cd my-app && npm run build)
```

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev my-app
```

**类型检查**：`momo:framework` 的类型来自 npm 包 `@momo/framework`（以 TS 源码发布；目前通过本地路径安装：`npm install --save-dev <momo 仓库>/framework/packages/momo`），在 `tsconfig.json` 里把 `momo:framework` 指过去：

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

**写给 Flutter 开发者**：API 形状与 Flutter 基本一致，主要差异：

- 构造参数用一个**选项对象**：`new Padding({ padding: EdgeInsets.all(8), child })`。
- 枚举多用字符串字面量：`mainAxisSize: "min"`、`crossAxisAlignment: "start"`。
- 没有 `runtimeType`：两个 View 可以复用同一 Element 的条件是**构造函数相同且 key 相等**。
- 没有 DOM API；网络、文件用 Node 的内置模块。UI 里的延迟任务用 `SchedulerBinding.instance.scheduleDelayed(ms, fn)`。
- 入口里同步调用 `runApp`，不要放在顶层 `await` 之后。

### 常用能力速查

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
| 无边框窗口 | `WindowChrome({ titleBarHeight, resizeBorder })` 隐藏系统标题栏；系统窗口控制按钮占用的区域见 `MediaQuery.of(ctx).windowControls`，两侧留白用 `windowControlsPadding`（macOS 在左、Windows 在右，一份代码适配）；哪里可以拖动窗口由应用决定：按下时调 `widget.startDrag(sequenceId)`（示例 `examples/webview/src/window_drag_area.ts`） |
| 网页 | 内置扩展 `momo:webview`（macOS）：`import { WebView } from "momo:webview"`（见 [extensions/webview/README.md](extensions/webview/README.md)） |
| 调用原生代码 | 原生扩展：Node.js 原生插件，以普通 npm 包的形式使用（[docs/07](docs/07-extension-system.md)） |

完整用法可参考 `examples/counter`、`examples/todo`、`examples/webview`，以及 `framework/test/` 下的测试用例。

### 不用框架：直接用 `momo:ui`

写游戏、可视化，或者想了解引擎边界时，可以绕开框架直接画帧。`momo:ui` 对标 Flutter 的 `dart:ui`，是框架与引擎之间的内部接口（跟着运行时的版本变），类型声明见 [momo-ui.d.ts](framework/packages/momo/src/ui/momo-ui.d.ts)。它只有引擎职责：`platformDispatcher` 是全局的帧调度与帧回调，`viewports` 是各窗口的画面、尺寸与输入；窗口标题等属于宿主，经 `momo/window` 通道操作。

一帧的写法：在 `platformDispatcher.onBeginFrame` 里用 `PictureRecorder` / `Canvas` 记录绘制指令，在 `onDrawFrame` 里用 `SceneBuilder` 组装场景并 `view.render(scene)`，要下一帧就 `platformDispatcher.scheduleFrame()`（不请求就不出帧，空闲零功耗）。完整的例子是 `examples/hello_frame/main.js`；这类脚本不需要构建：`Momo --dev path/to/main.js`。

---

## 调试

- **Chrome DevTools**：`--dev` 启动时在 `127.0.0.1:9229` 开启 Node 的 inspector（在 `chrome://inspect` 里添加 `localhost:9229`）：断点、console、性能分析、堆快照。`--inspect-brk` 在执行入口前等待调试器；`--inspect-port=<n>` 换端口。
- **日志**：`console.*` 写到 stdout / stderr，从终端启动 `Momo.app/Contents/MacOS/Momo` 即可看到；连着 DevTools 时也出现在它的控制台。
- **异常**：build 中抛出的异常让对应子树显示为 `ErrorView`；布局、事件处理中的异常被报告并跳过，不会让应用崩溃。
- **软件渲染**：`MOMO_SOFTWARE_RENDERING=1` 强制 CPU 光栅（排查 GPU 相关问题）。

## 打包

`momo build` 把应用打包成可分发的 `.app`（可选 DMG）：复制运行时，再把应用目录里的 `package.json`、`main`、`assets/**`（加 `momo.include`）原样放进去。它不编译：先用应用自己的构建脚本产出 `main`。

```bash
node framework/packages/cli/momo.mjs build my-app --out=out/apps --runtime=out/debug/embedder/Momo.app --dmg
```

CLI 是 npm 包 `@momo/cli`（命令 `momo`）；运行时用 `--runtime=` 或环境变量 `MOMO_RUNTIME` 指定，发布时请用 Release 构建的运行时。全部选项、`package.json` 的 `momo.*` 字段、带哪些文件的规则见 [docs/09 §4.3](docs/09-build-and-deps.md)。

## 原生扩展

框架内置的 API 不够用时写**原生扩展**：它就是 Node.js 原生插件（`.node`，Node-API）+ JS 门面，以普通 npm 包的形式提供；需要引擎能力（平台线程、外部纹理、平台视图）的扩展另用一张很小的宿主能力表。内置扩展随运行时提供（目前只有 macOS 上的 [`momo:webview`](extensions/webview/README.md)）。怎么写、怎么在应用里使用、怎么打包见 [docs/07-extension-system.md](docs/07-extension-system.md)，示例在 `extensions/examples/`。

## 命令行参数与环境变量

`Momo [<app>] [--key=value ...]`（未知参数与 `-psn_*`、`-NS*` 等系统参数会被忽略）：

| 参数 | 说明 |
|---|---|
| `<app>` | 应用路径：目录（入口是其 `package.json` 的 `main`）或一个 JS 文件。默认是应用包内的 `Resources/app` |
| `--title=<text>`、`--width=<px>`、`--height=<px>` | 启动窗口的标题与尺寸（逻辑像素） |
| `--no-startup-window` | 不创建启动窗口（由应用自己 `Widget.create`） |
| `--dev` | 开发模式：启用 inspector，不用编译缓存 |
| `--inspect-port=<n>` | inspector 端口（默认 9229，`-1` 关闭） |
| `--inspect-brk` | 执行入口前等待调试器连接（等待期间窗口照常响应） |

| 环境变量 | 说明 |
|---|---|
| `MOMO_SOFTWARE_RENDERING=1` | 强制使用 CPU 软件渲染 |
| `MOMO_RUNTIME` | `momo build` 使用的运行时 |

## 示例

| 示例 | 层次 | 内容 |
|---|---|---|
| `examples/hello_frame` | `momo:ui` | 不用框架直接画动画帧；默认打进 `Momo.app`，无需构建 |
| `examples/counter` | View | Material 计数器：按钮、主题、键盘快捷键 |
| `examples/todo` | View | 输入框、添加/勾选/删除、列表滚动 |
| `examples/webview` | View + `momo:webview` | 多窗口、多标签浏览器（macOS）：无边框窗口、标签拖拽、应用菜单 |
