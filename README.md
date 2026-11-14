# Momo — 桌面端自绘跨端框架

Momo 是一个面向 macOS 与 Windows 的**自绘**（self-rendered）桌面应用框架。你用 TypeScript/JavaScript 编写界面和业务逻辑，产物是一个原生应用（macOS 上是 `.app`）。所有像素都由 Skia 绘制，不依赖系统控件，因此双端外观一致。

分层借鉴 Flutter（Framework / Engine / Embedder），但框架层用 TypeScript 而不是 Dart：

```
应用代码（TS）  ──  runApp(new MaterialApp({ home: ... }))
Framework（TS） ──  View / Element / RenderObject、布局、手势、动画、Material 控件；app / Widget（应用与窗口）
Engine（C++）   ──  V8 · momo:ui 绑定 · DisplayList / LayerTree · Skia（Metal / 软件）· 文本 · 扩展宿主
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
| Node.js + npm | 构建框架与应用 bundle（esbuild、TypeScript） |

### 1. 拉取预编译依赖

Skia 与 V8 使用预编译包（版本与 SHA256 锁定在 `cmake/deps.lock.json`），下载到 `third_party/prebuilt/`：

```bash
python3 tools/fetch_deps.py
```

### 2. 构建运行时 Momo.app

```bash
cmake -S . -B out/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

```bash
cmake --build out/debug
```

产物 `out/debug/embedder/Momo.app` 是**通用运行时**：它只负责创建窗口、启动引擎并执行一个 JS bundle。默认打包进去的 bundle 是 `examples/hello_frame/main.js`，直接打开就能看到一个动画画面：

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo
```

### 3. 安装框架依赖

```bash
cd framework && npm install
```

### 4. 运行一个 View 示例

```bash
node framework/scripts/build_example.mjs examples/counter/src/main.ts examples/counter/dist/main.js
```

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev --bundle=examples/counter/dist/main.js
```

---

## 编写第一个应用

一个 Momo 应用就是一个普通的 npm 工程，入口默认是 `src/main.ts`：

```
my-app/
├── package.json
├── src/
│   └── main.ts
└── assets/            # 可选：图片等资源，运行时通过 AssetImage("logo.png") 等访问
```

`package.json`（`@momo/framework` 目前通过本地路径引用；`momo.name` 是打包后的 `.app` 名称）：

```json
{
  "name": "my-app",
  "private": true,
  "type": "module",
  "main": "src/main.ts",
  "momo": { "name": "MyApp" },
  "dependencies": {
    "@momo/framework": "file:../momo-djs/framework/packages/momo"
  }
}
```

`src/main.ts`：

```ts
import {
  Center, Column, ElevatedButton, MaterialApp, Scaffold, AppBar, SizedBox,
  State, StatefulView, Text, runApp, type BuildContext, type View,
} from "@momo/framework";

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

### 开发循环：构建 bundle + 热重启

把 TS 打成单文件 ESM bundle（只保留 `momo:*` / `ext:*` 外部导入，引擎会以内置模块提供），再用 `--dev` 启动运行时：

```bash
node framework/scripts/build_example.mjs my-app/src/main.ts my-app/dist/main.js
```

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev --bundle=my-app/dist/main.js
```

`--dev` 下运行时每 500ms 检查一次 bundle 文件，**重新构建后窗口会自动 hot restart**（重建 JS 运行时、从根重新构建界面，不保留 JS 状态）。新 bundle 有错误时会退回占位画面，修好后再次构建即可恢复。

### 写给 Flutter 开发者

API 形状与 Flutter 基本一致，主要差异：

- 构造参数用一个**选项对象**：`new Padding({ padding: EdgeInsets.all(8), child })`，而不是命名参数。
- 枚举多用字符串字面量：`mainAxisSize: "min"`、`crossAxisAlignment: "start"`。
- 颜色是 `new Color(0xff4285f4)` 或 `Colors.*`。
- 没有 `runtimeType`：两个 View 可以复用同一 Element 的条件是**构造函数相同且 key 相等**。
- 运行环境里没有 DOM / Node API（没有 `fetch`、`setTimeout`），延迟任务请用 `SchedulerBinding.instance.scheduleDelayed(ms, fn)`，网络、文件等能力由扩展提供。

---

## 两种写法：View 框架与 momo:ui 底层 API

绝大多数应用用 View 框架（上一节）。如果你要写游戏、可视化，或者想了解引擎边界，也可以绕开框架、直接用引擎提供的 `momo:ui` 模块画帧。它对标 Flutter 的 `dart:ui`，完整类型声明见 [framework/packages/momo/src/ui/momo-ui.d.ts](framework/packages/momo/src/ui/momo-ui.d.ts)。

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

这类脚本不需要构建，直接运行：`Momo --dev --bundle=path/to/main.js`。更完整的例子（含经 `momo/window` 通道设置窗口标题）见 `examples/hello_frame`。

---

## 常用能力速查

以下 API 都从 `@momo/framework` 导入。

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
| 无边框窗口 | `WindowChrome({ titleBarHeight, resizeBorder })` 隐藏系统标题栏；系统窗口控制按钮占用的区域见 `MediaQuery.of(ctx).windowControls`，两侧留白用 `windowControlsPadding`（macOS 在左、Windows 在右，一份代码适配）；`WindowDragArea` 声明可拖动窗口的区域 |
| 窗口能力 | `widget.capabilities`、`widget.bounds`、`widget.state`（平台不支持的能力是空操作，可先查询） |
| 与原生通信 | `new MethodChannel("acme/foo").invokeMethod("bar", args)`；对端由原生扩展注册（见下文） |

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

`tools/momo-cli` 把应用打包成可分发的 `.app`（可选 DMG）。它会：用 esbuild 打 bundle → 收集依赖中声明的原生扩展、生成 `extensions.manifest` 并拷贝动态库 → 复制预编译的运行时 `Momo.app` 为 `<Name>.app`，并把 bundle、`assets/`、扩展放进 `Contents/Resources/`。

```bash
node tools/momo-cli/momo.mjs build my-app --out=out/apps
```

```bash
node tools/momo-cli/momo.mjs build my-app --out=out/apps --minify --dmg
```

| 选项 | 说明 |
|---|---|
| `--out=<dir>` | 输出目录 |
| `--name=<App>` | 应用名（默认取 `package.json` 的 `momo.name`，其次是 `name`） |
| `--runtime=<Momo.app>` | 使用的运行时，默认 `out/debug/embedder/Momo.app`（发布时请用 Release 构建的运行时） |
| `--native-dir=<dir>` | 到哪里找扩展的原生库，默认 `out/debug/extensions` |
| `--minify` | 压缩 bundle |
| `--dmg` | 额外用 `hdiutil` 产出 `<Name>.dmg` |

打包后的应用资源布局：

```
MyApp.app/Contents/
├── MacOS/Momo                   # 运行时可执行文件
├── Frameworks/libnode.*.dylib     # Node.js(含 V8)
└── Resources/
    ├── bundle.js                # 你的应用
    ├── assets/                  # 来自 my-app/assets
    └── extensions/              # extensions.manifest + 原生扩展动态库
```

非 `--dev` 模式下，引擎会把 JS 编译结果缓存在 `~/Library/Caches/dev.momo.app/v8`，加快之后的启动。

---

## 原生扩展

框架内置的 API 不够用时（数据库、托盘、音视频、硬件、现有 C/C++ SDK……），可以写**原生扩展**：一个 JS 包 + 一个动态库。动态库只依赖扩展 SDK 的头文件：契约是稳定的纯 C ABI [momo_extension.h](extensions/sdk/include/momo/momo_extension.h)，扩展与宿主可以用不同的编译器构建；C++ 扩展可以用其上的便利封装 [momo_extension.hpp](extensions/sdk/include/momo/momo_extension.hpp)。

### 扩展 SDK（构建产物）

构建 Momo 时，SDK 会原样放到 `out/<build>/sdk/`，也可以安装到任意目录：

```bash
cmake --install out/debug --prefix ~/momo-sdk --component sdk
```

```
sdk/
├── include/momo/momo_extension.h     # 稳定 C ABI（唯一契约）
├── include/momo/momo_extension.hpp   # C++17 封装（header-only）
├── lib/cmake/MomoExtSdk/             # find_package(MomoExtSdk) 的包配置
└── types/ext.d.ts                    # ext:* 模块的 TS 类型模板
```

扩展工程的 CMake 只需要两行，`momo_add_extension()` 会设置隐藏可见性，保证只导出 `momo_ext_init`：

```cmake
find_package(MomoExtSdk 1 REQUIRED)        # -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk
momo_add_extension(ext_acme src/ext_acme.cc)
```

### 使用扩展

扩展对应用来说就是一个普通 npm 依赖：

```json
"dependencies": {
  "@momo-examples/ext-hello": "file:../momo-djs/extensions/examples/hello-extension"
}
```

```ts
import { add, greet, hash } from "@momo-examples/ext-hello";
console.log(add(1, 2), greet("Momo"), await hash(new Uint8Array([1, 2, 3])));
```

`momo build` 会扫描依赖中 `package.json` 的 `momo.extension` 声明，自动把原生库打进应用，并打印每个扩展申请的能力（capabilities）。扩展本身的示例见 `extensions/examples/`。

### 编写扩展

```bash
node tools/momo-cli/momo.mjs ext create my-ext --id=acme.myext
```

生成的骨架包含 C 模板、TS 门面和 CMake 文件。用 C++ 写时，继承 `momo::ext::Extension`，用 `MOMO_EXTENSION` 宏导出入口：

```cpp
#include "momo/momo_extension.hpp"

class MyExt final : public momo::ext::Extension {
 public:
  void OnAttach(momo::ext::Host& host) override {                 // platform 线程
    host.RegisterModule("ext:myext", [](momo::ext::ModuleBuilder& m) {
      m.Function("add", [](momo::ext::Call& call) {               // ui 线程调用，禁止阻塞
        auto a = call.Number(0), b = call.Number(1);
        if (!a || !b) return call.Throw("add(a, b) expects two numbers");
        call.Return(*a + *b);
      });
    });
    host.SetChannelHandler("myext/echo", kMomoRunnerWorkerPool,   // 在 worker pool 上处理
                           [&host](momo::ext::Bytes bytes, uint64_t reply) {
                             host.Reply(reply, bytes.data, bytes.size);
                           });
  }
  void OnDetach() override {}                                     // 停止自己的线程与定时任务
};

MOMO_EXTENSION(MyExt, "acme.myext")
```

扩展内部可以使用异常，但异常不得穿过 C ABI 边界（注册给宿主的 lambda 里必须捕获）。完整的 C++ 示例见 `extensions/examples/texture-extension`。

用纯 C 写时，直接使用 C ABI：导出 `momo_ext_init`，在 `on_attach` 里注册 JS 模块与消息通道：

```c
#include "momo/momo_extension.h"

static const MomoHostApi* g_host;

static void Add(void* user_data, MomoCallInfoRef call) {          // ui 线程调用，禁止阻塞
  double a, b;
  g_host->call->arg_double(call, 0, &a);
  g_host->call->arg_double(call, 1, &b);
  g_host->call->return_double(call, a + b);
}

static void SetupModule(void* user_data, MomoJsBuilderRef b) {
  g_host->builder_add_function(b, "add", Add, NULL);
}

static void OnAttach(void* user_data, const MomoHostApi* host, MomoEngineRef engine) {   // platform 线程
  g_host = host;
  host->register_module(engine, "ext:myext", SetupModule, NULL);   // JS: import { native } from "ext:myext"
}

static void OnDetach(void* user_data) { g_host = NULL; }            // 引擎关闭前释放一切宿主资源

MOMO_EXT_EXPORT bool momo_ext_init(uint32_t host_api_version, MomoExtension* out) {
  if (host_api_version < MOMO_EXT_API_VERSION) return false;
  out->id = "acme.myext";
  out->on_attach = OnAttach;
  out->on_detach = OnDetach;
  return true;
}
```

宿主 API 还提供：Promise（在 worker 线程完成后 `promise_resolve_*`）、消息通道（`channel_set_handler` 可指定 platform / ui / io / worker pool 线程）、任务投递、窗口句柄（按视图 id 取得 `NSView*`，用于托盘、子窗口等系统集成）、帧回调、纹理注入（扩展自己生成像素，在界面中用 `Texture` widget 显示）。

**回调线程规则**：宿主只在你注册回调的那个线程上调用它，或者在注册时用 `MomoTaskRunner` 显式指定的线程上调用；不会在宿主内部线程上调用扩展代码。因此帧回调（`add_frame_callback`）只能在 ui 线程注册；acquire/release 方式的纹理必须在注册线程上调用 `texture_mark_frame_available`，否则返回 `kMomoErrWrongThread`。

两个示例：

| 示例 | 演示 |
|---|---|
| `extensions/examples/hello-extension` | 纯 C：同步函数、字符串/字节、worker 线程 + Promise、消息通道 |
| `extensions/examples/texture-extension` | C++ 封装：纹理注入，帧回调驱动 worker 渲染动画并推帧 |

`node tools/momo-cli/momo.mjs ext verify <lib>` 检查动态库只导出 `momo_ext_init`、不链接引擎内部库。设计细节见 [docs/07-extension-system.md](docs/07-extension-system.md)。

---

## 命令行参数与环境变量

运行时 `Momo` 的参数（未知参数与 `-psn_*`、`-NS*` 等系统参数会被忽略）：

| 参数 | 说明 |
|---|---|
| `--bundle=<path>` | 要执行的 JS bundle，默认 `<Resources>/bundle.js` |
| `--assets=<dir>` | 资源目录，默认 `<Resources>/assets` |
| `--title=<text>` | 初始窗口标题 |
| `--width=<px>` / `--height=<px>` | 初始窗口尺寸（逻辑像素） |
| `--dev` | 开发模式：bundle 修改后 hot restart、启用 Inspector、bundle 缺失也能启动 |
| `--inspect-port=<n>` | Inspector 端口（默认 9229，`-1` 关闭） |
| `--inspect-brk` | 执行 bundle 前等待调试器连接 |

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

View 示例都是独立 npm 工程，可以用 `build_example.mjs` 构建后以 `--bundle` 运行，也可以用 `momo build` 打包成 `.app`。

---

## 仓库结构与文档

| 目录 | 内容 |
|---|---|
| `framework/` | TypeScript UI 框架（`@momo/framework`）及其测试 |
| `engine/` | 核心引擎（C++）：V8 运行时、`momo:*` 绑定、渲染管线、文本、资源、扩展宿主；`engine/platform/` 是宿主需要实现的平台契约 |
| `embedder/` | 宿主层：进程入口、生命周期、Engine 装配；`common/` 平台无关，`macos/` / `windows/` / `headless/` 为各平台实现 |
| `base/` | C++ 基础库：Object 模型、EventLoop、TaskRunner、Status |
| `extensions/` | 扩展 SDK（C ABI 头 + JS 类型）与示例扩展 |
| `examples/` | 示例应用 |
| `tools/` | `fetch_deps.py`（依赖下载）、`momo-cli`（打包、扩展脚手架） |
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
- **Windows**：平台层（与 macOS 同一套指针派发、窗口能力、应用生命周期）与 D3D12 GPU 后端已实现，但**未在 Windows 上编译运行验证**（macOS 上只做了单测与交叉编译检查）；不支持平台视图。
- 技术选型：C++20（禁用异常与 RTTI）· V8 · Skia（macOS 走 Metal，CPU 光栅为回退）· SkParagraph（含 ICU）· CMake + Ninja。

逐项状态见 [docs/11-roadmap.md](docs/11-roadmap.md)。
