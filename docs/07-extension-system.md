# 07 · 原生扩展

框架内置的 API 永远不够（数据库、音视频、硬件、现有 C/C++ SDK…）。Momo 的 JS 运行在 Node.js 环境里（ARCHITECTURE.md §4.6），所以**原生扩展就是 Node.js 原生插件**（`.node`，Node-API）：npm 上现成的原生包按设计直接可用，自己写的扩展也用同一套工具。Momo 只补上 Node-API 给不了的东西——引擎自己的能力（平台线程、外部纹理、平台视图），以一张很小的**宿主能力表**（稳定 C ABI）提供。

本文是扩展的唯一文档：形态（§1）、加载与打包（§2）、宿主能力表（§3）、关闭顺序（§4）、SDK（§5）、从零写一个（§6）。引擎内部怎么实现见 ARCHITECTURE.md §4.9、§4.10。

## 1. 扩展的形态

一个扩展 = **JS 门面 + 一个 `.node` 文件**。按 JS 从哪里来分两种，原生一侧（Node-API、宿主能力表、SDK）完全相同：

| | 内置扩展 | 三方扩展 |
|---|---|---|
| 是什么 | 运行时的一部分，随运行时发布 | 应用的依赖，属于业务代码 |
| 在哪里 | `<resources>/builtin/<name>/`：JS 与 `.node` 在同一个目录 | 应用的 `node_modules/<包>/` |
| 怎么导入 | `import { WebView } from "momo:webview"` | `import { Database } from "@acme/ext-sqlite"` |
| 应用要做什么 | 什么都不用做 | 自己安装、自己决定怎么带进应用包（§2） |
| 现有的 | `momo:webview`（macOS WKWebView、Windows WebView2；源码 `extensions/webview/`，接口见其 README） | 示例 `extensions/examples/{hello,texture}-extension` |

**JS 与原生之间只有 Node-API 一条路**：函数、对象、类、Promise、`napi_async_work`（线程池）、`napi_threadsafe_function`（从别的线程回到 JS）。不经过消息通道，也没有 Momo 专有的参数/返回值 API。

内置扩展的源码只有 `js/index.ts`（单入口）与 `native/`，没有 `package.json`：构建运行时的时候 JS 被编译成 `builtin/<name>/index.js`，插件直接输出到同一个目录（docs/09 §4.1），JS 里写 `import addon from "./ext_webview.node"`。内置扩展里的 `import "momo:framework"` 与应用拿到的是同一份框架，所以它可以直接提供视图。

## 2. 加载与打包

没有 manifest，没有宿主侧的扩展列表：扩展由 JS 加载，加载失败就是一次普通的 `import` 失败（异常里有原因）。不做"能力声明"与加载白名单：JS 本来就能 `process.dlopen`、读写文件、起子进程，宿主拦不住，就不假装拦。

```
内置: import { WebView } from "momo:webview"
        → 模块钩子: <builtin_dir>/webview/package.json 的 main → index.js → import addon from "./ext_webview.node"
三方: import { Database } from "@acme/ext-sqlite"
        → Node 的解析规则: 应用的 node_modules → 包的 js/index.js → import addon from "../build/ext_sqlite.node"
 → 运行时的模块钩子(engine/runtime/node_bootstrap.cc)把 .node 交给 Node 的 require → dlopen → NAPI_MODULE_INIT
 → 需要宿主能力的门面: 把 nativeHost 传给插件                         [全部在 ui(JS)线程]
```

Node 自己只在 `require` 里认 `.node`；Momo 的模块钩子让 ESM 的 `import x from "./x.node"` 也能用（默认导出是插件的 exports）。

**三方扩展是业务代码，Momo 不关心**：它怎么安装、怎么进应用的产物、怎么带进应用包，都是应用自己的事，Momo 对此没有任何专门的配置项或概念。一个三方包只有两种去处：

- **编译进应用的 `main`**：纯 JS 的包可以这样，由应用自己的构建工具完成；
- **保持原样留在 `node_modules` 里**，运行时由 Node 加载。**带原生插件的包必须这样**：包内的 JS 按相对路径找自己的 `.node` 与它依赖的动态库，只有整包原样、文件的相对位置不变才找得到。

留在 `node_modules` 里的包，应用要做两件事，用的都是通用的手段：

```json
// 应用的 package.json
"scripts": {
  "build": "esbuild src/main.ts --bundle --format=esm --platform=neutral '--external:momo:*' --external:@acme/ext-sqlite --outfile=dist/main.js"
},
"momo": { "include": ["node_modules/@acme/ext-sqlite/"] }
```

1. 在**自己的构建配置**里把它排除（esbuild：`--external:<包名>`），导入在 `main` 里原样保留；
2. 打包时用 `momo.include` 把 `node_modules/<包>/` 带进应用包（docs/09 §4.3；`momo.include` 是通用的"追加文件"，不是扩展专用的）。它自己还依赖别的包时，把那些包也写上；用不到的文件可以用 `momo.ignore` 去掉。

开发时（`Momo --dev <应用目录>`）和打包后是同一条路：Node 从应用目录的 `node_modules` 加载这个包。

- 留在 `node_modules` 里的包必须是 JS（运行时不执行 TS），类型写在旁边的 `.d.ts` 里。
- **需要框架的三方扩展**（带视图的）直接 `import "momo:framework"`：它由运行时提供，与应用拿到的是同一份，所以这样的扩展也是一个保持原样的整包。
- **npm 上现成的原生包**按设计直接可用，包括用 `node-gyp-build` 在运行期拼路径找 `.node` 的。**没有拿真实的 npm 原生包测过**，只测了仓库里的示例。

## 3. 宿主能力表（`extensions/sdk/include/momo/momo_host.h`）

只有用到引擎能力的扩展才需要它。JS 侧 `import { nativeHost } from "momo:services"`（一个 BigInt：表的地址；不用 External——Node 26 上 `napi_get_value_external` 读 V8 原生的 External 会崩溃），原样传给插件，插件用头文件里的 `momo_host_from_napi(env, value)` 取出 `const MomoHost*`。

| 函数 | 调用线程 | 说明 |
|---|---|---|
| `post_platform_task(host, fn, user_data)` | 任意 | 在 platform（OS 主）线程执行 `fn`；按投递顺序。原生 UI 只能在那里用 |
| `texture_create(host, &id)` | 任意 | 创建外部纹理，得到 id |
| `texture_update_pixels(host, id, rgba, w, h, row_bytes)` | 任意 | 推一帧 RGBA8（非预乘）：宿主拷贝，并请求引擎出一帧 |
| `texture_destroy(host, id)` | 任意 | 销毁 |
| `platform_view_register_factory(host, type, factory)` | 任意 | 注册视图类型（`"acme.webview"`；`momo/` 前缀保留）。`create` / `dispose` 回调在 platform 线程 |

- 纯 C、无异常；全部返回 `MomoStatus`。表带 `struct_size` 与 `api_version`，**只在末尾追加**，已有字段不改不删；插件用 `struct_size` 判断宿主是否提供某个函数。
- 指针在进程存续期间一直有效，可以保存、可以在任意线程使用；引擎关闭后各函数返回 `kMomoErrClosed`。
- **宿主只在 platform 线程调用扩展**，而且只有两类回调：平台任务、视图工厂。
- 这张表里没有、也不需要的东西：回到 JS 线程用 `napi_threadsafe_function`；后台线程用 `napi_async_work` 或自己开；读应用资源用 `momo:services` 的 `assetsDir` 或 `node:fs`；跟随帧节奏由 JS 驱动（框架的 Ticker）或插件自己定时。

**外部纹理**是"扩展借助引擎绘制能力"的途径：扩展不直接拿 SkCanvas（Skia 的 C++ ABI 不稳定），而是在自己的线程生成像素并推给纹理，JS 侧用框架的 `Texture` 视图（或 `SceneBuilder.addTexture`）按 id 显示，引擎以 `TextureLayer` 合成。目前只有 CPU 像素帧。

**平台视图**用于"本身就是系统视图、需要原生输入 / IME / 无障碍"的内容（WebView、地图、系统控件）。扩展只负责创建与释放视图；嵌入的难点（分片合成、同帧呈现、命中遮挡、焦点交接）全部在引擎与 embedder 里（ARCHITECTURE.md §4.10）。JS 侧用框架的 `PlatformView`。支持的 surface：macOS 的 Metal、Windows 的 Direct3D（软件回退后视图不显示）。`create` 返回的原生视图归扩展所有，每次成功的 `create` 恰好对应一次 `dispose`：

- **macOS**：一个 `NSView*`（惯用 `CFBridgingRetain` / `CFBridgingRelease`），宿主把它挂进窗口的视图层级，事件经 AppKit 投递。
- **Windows**：一个 `MomoWinPlatformView*`（头文件里的一组回调）。内容经 DirectComposition 合成：宿主 `attach` 时给出顶层 `HWND` 与一个 `IDCompositionVisual` 挂载点，视图把自己的 visual 挂在它下面（WebView2 的 visual hosting 正是这种形态）；位置、裁剪、透明度、z 序由宿主负责。宿主决定指针的接收方后经 `mouse` 交来 Win32 鼠标消息（视图内容坐标），向 `cursor` 取光标，经 `focus` 交出键盘焦点；视图经宿主给的 `MomoWinPlatformViewSite` 请求提交 visual 树（`commit`）、报告用户点击取得焦点（`focused`）、光标变化（`cursor_changed`），持有焦点时的按键先交 `key_down`（应用菜单的快捷键优先）。

## 4. 关闭

扩展的代码随 JS 运行时一起结束（Node 在环境销毁时运行插件登记的清理函数），所以引擎对扩展的调用必须先结束。`Shell::Shutdown`（platform 线程）的顺序固定：

```
① 关闭宿主能力表: 此后每个函数返回 kMomoErrClosed;已投递而未执行的平台任务被丢弃(不会执行)
② dispose 全部平台视图(先移出窗口, 再调工厂的 dispose), 清空工厂            ← 此后引擎不再调用任何扩展代码
③ 销毁 JS 运行时: 扩展用 napi_add_env_cleanup_hook 登记的清理函数在这里执行
④ raster: 释放 GPU 资源
```

扩展作者要记住的：

- **清理函数执行时，宿主已不会再调用你**。在清理函数里停掉自己的线程、关掉 `napi_threadsafe_function`。
- ② 时 JS 还在运行，可能对已经 dispose 的视图 id 发命令：按 id 查视图，查不到就报错，不缓存指针。
- ① 丢弃的平台任务不会调用 `fn`，它的 `user_data` 也就不被释放。关闭只发生在进程退出之前，这部分内存随进程回收；但不要让 `user_data` 持有必须显式归还的东西（文件锁、要落盘的数据）。
- 一个进程里只有一个 JS 运行时（没有热重启），所以扩展里"属于 JS"的状态与进程同寿，没有别的生命周期事件。系统框架自己的在途回调（如 WebKit 的 completion handler）可能晚于 ③ 才到：扩展要自己保证此时不再碰 JS。

## 5. SDK

SDK = 编译一个能被 Momo 加载的插件所需的全部。构建 Momo 时放到 `out/<build>/sdk/`，也可 `cmake --install <build> --prefix <dir> --component sdk`；发布产物是 `momo-ext-sdk-v<version>-<platform>.zip`。

| 路径 | 内容 |
|---|---|
| `include/momo/momo_host.h` | 宿主能力表（§3） |
| `include/node/` | **Momo 运行时所带 Node 的头文件**（`node_api.h` 等；同 Electron 发布自己的头文件）。不含 openssl |
| `Release/node.lib` | 仅 Windows：`libnode.dll` 的导入库（路径同 node-gyp 的 `--nodedir` 约定） |
| `lib/cmake/MomoExtSdk/` | `find_package(MomoExtSdk)`：`momo::ext_sdk` 与 `momo_add_extension()` |

```cmake
find_package(MomoExtSdk 1 REQUIRED)        # -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk
momo_add_extension(ext_acme src/acme.cc)   # CMakeLists.txt 在 <包>/native/ → 产物 <包>/build/ext_acme.node
```

- `momo_add_extension()` 认变量 `MOMO_EXTENSION_OUTPUT_DIR`：设了就输出到那里（内置的 webview 用它把插件输出到运行时的 `builtin/webview/`）。
- **符号从哪来**：Node-API 的符号在 libnode 里，不在应用的可执行文件里。macOS 上插件以 `-undefined dynamic_lookup` 链接，加载时从进程解析。Windows 上插件直接链接 SDK 里的导入库，导入表指向 `libnode.dll`。
- Momo 用的是未打补丁、默认配置的 Node（版本见 docs/09 §2），所以 Node-API 与 Node 自身的 ABI 都和同版本的官方 Node 一致；只用 Node-API 的插件不随 Node 版本变化。
- **未验证**：用 node-gyp 构建（`node-gyp rebuild --nodedir=<sdk>`）；npm 上预编译的插件；全部 Windows 路径。

## 6. 写一个三方扩展

照着 `extensions/examples/hello-extension` 写。它是一个普通的 npm 包：

```
ext-myext/
├── package.json          # type: module, main: js/index.js, types: js/index.d.ts;没有 Momo 专有的声明
├── js/index.js           # JS 门面: 按相对路径 import 插件(必须是 JS: 运行时不执行 TS)
├── js/index.d.ts         # 门面的类型
├── native/
│   ├── CMakeLists.txt    # §5 那两行
│   └── src/myext.c       # Node-API 模块
└── build/myext.node      # 编译产物(不入库)
```

```c
// native/src/myext.c
#include <node_api.h>

static napi_value Add(napi_env env, napi_callback_info info) {     // JS(ui)线程调用, 禁止阻塞
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

```js
// js/index.js
import addon from "../build/myext.node";   // 相对路径, 指向包内;默认导出 = 插件的 exports
export const { add } = addon;
```

```bash
cmake -S native -B native/out -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build native/out
```

应用里 `npm install <包>`，按 §2 排除并 include，然后 `import { add } from "ext-myext"`。插件内部可以使用异常，但异常不得穿过 Node-API 与宿主表的回调边界。需要引擎能力时，门面把 `nativeHost` 传给插件（§3）——`texture-extension` 是例子。

| 位置 | 内容 |
|---|---|
| `extensions/examples/hello-extension` | 纯 C，只用 Node-API（不需要宿主表）：同步函数、字符串/字节、线程池上的 Promise |
| `extensions/examples/texture-extension` | C++：外部纹理，自己的线程按约 60 fps 推帧；清理函数里停线程 |
| `extensions/webview` | 内置扩展 `momo:webview`。平台无关的插件核心（`webview_extension.cc`）+ WKWebView（macOS）/ WebView2（Windows）两个实现；命令经 `post_platform_task` 到主线程，事件经 `napi_threadsafe_function` 回 JS。示例应用 `examples/webview` |
| `extensions/testing/lifecycle-addon` | 只供测试：记录宿主对它的每次调用，验证 §4 的关闭顺序 |

测试：`engine/native_host/native_host_unittests.cc`（宿主能力表，只经 C 函数表调用）；`extensions/examples/examples_unittests.cc`（端到端：真实的引擎执行 JS，JS import 真实的 `.node`）；CTest `ExtensionSdk.StandaloneCxxBuild`（只凭 `<build>/sdk` 独立配置并编译 texture 示例）。

## 7. 为什么不是自有的扩展 ABI

自有的扩展机制（专用入口、manifest 声明式加载、自有的 JS 调用参数表、扩展消息通道）在接入 Node.js 之后一大半与 Node-API 重复，而且弱得多，还挡住了 npm 上现成的原生包；"静态声明式加载更安全"的理由也不成立（JS 能 `process.dlopen`）。所以只留下引擎能力这一小块，就是 §3 的那张表。
