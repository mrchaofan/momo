# 07 · 原生扩展

一方暴露的 JS API 永远不够(数据库、音视频、托盘、硬件、现有 C/C++ SDK…)。Momo 的 JS 运行在 Node.js 环境里
(docs/04), 所以**原生扩展就是 Node.js 原生插件**(`.node`, Node-API): npm 上现成的原生包直接可用, 自己写的扩展
也用同一套工具。Momo 只补上 Node-API 给不了的东西——引擎自己的能力(平台线程、外部纹理、平台视图), 以一张很小的
**宿主能力表**(稳定 C ABI)提供。

## 1. 扩展的形态

一个扩展 = 一个普通的 npm 包: **JS 门面 + 一个 `.node` 文件**。

```
ext-sqlite/
├── package.json          # 普通的 package.json, 没有 Momo 专有的声明
├── js/index.ts           # JS 门面: import 插件, 给它的函数加上类型, 提供好用的 API
├── native/
│   ├── CMakeLists.txt    # find_package(MomoExtSdk) + momo_add_extension(ext_sqlite ...)
│   └── src/ext_sqlite.cc # Node-API 模块(NAPI_MODULE_INIT)
└── build/ext_sqlite.node # 编译产物(node-gyp 的惯例位置;不入库)
```

```ts
// ext-sqlite/js/index.ts
import addon from "../build/ext_sqlite.node";     // 默认导出 = 插件的 exports
const native = addon as { open(path: string): Promise<number> };
export class Database {
  static async open(path: string): Promise<Database> { ... }
}
```

对应用开发者而言与任何 npm 包无差别: `npm i @acme/ext-sqlite` + `import { Database } from "@acme/ext-sqlite"`。

**JS 与原生之间只有 Node-API 一条路**: 函数、对象、类、Promise、`napi_async_work`(线程池)、
`napi_threadsafe_function`(从别的线程回到 JS)。不经过消息通道, 也没有 Momo 专有的参数/返回值 API。

## 2. 加载

没有 manifest, 没有宿主侧的扩展列表: 扩展由 JS 加载, 加载失败就是一次普通的 `import` 失败(异常里有原因)。

```
JS: import addon from "./x.node"
 → 运行时的模块钩子(engine/runtime/node_bootstrap.cc)把 .node 交给 Node 的 require → dlopen → NAPI_MODULE_INIT
 → 需要宿主能力的门面: native.attach(nativeHost, …)        [全部在 ui(JS)线程]
```

- Node 自己只在 `require` 里认 `.node`;Momo 的模块钩子让 ESM 的 `import x from "./x.node"` 也能用(默认导出是
  插件的 exports)。类型声明(`declare module "*.node"`)在 `@momo/framework` 里。
- **打包**(`momo bundle` / `momo build`, docs/09 §4): 按路径 import 的 `.node` 被拷到 `<out>/native/`, bundle 里的
  导入改为 `./native/<name>.node`。`.node` 不存在(扩展还没编译)时打包报错。只认静态的按路径 import;运行期自己
  拼路径找 `.node` 的第三方包(如用 `node-gyp-build` 的)不会被拷贝, 这类包不要打进 bundle。
- 不做"能力声明"与加载白名单: JS 本来就能 `process.dlopen`、读写文件、起子进程, 宿主拦不住, 就不假装拦。

## 3. 宿主能力表(extensions/sdk/include/momo/momo_host.h)

只有用到引擎能力的扩展才需要它。JS 侧 `import { nativeHost } from "momo:services"`(一个 BigInt: 表的地址),
原样传给插件, 插件用头文件里的 `momo_host_from_napi(env, value)` 取出 `const MomoHost*`。

| 函数 | 调用线程 | 说明 |
|---|---|---|
| `post_platform_task(host, fn, user_data)` | 任意 | 在 platform(OS 主)线程执行 `fn`;按投递顺序。原生 UI 只能在那里用 |
| `texture_create(host, &id)` | 任意 | 创建外部纹理, 得到 id |
| `texture_update_pixels(host, id, rgba, w, h, row_bytes)` | 任意 | 推一帧 RGBA8(非预乘): 宿主拷贝, 并请求引擎出一帧 |
| `texture_destroy(host, id)` | 任意 | 销毁 |
| `platform_view_register_factory(host, type, factory)` | 任意 | 注册视图类型(`"acme.webview"`;`momo/` 前缀保留)。`create` / `dispose` 回调在 platform 线程 |
| `platform_view_get(host, view_id, &native)` | platform | 按 id 取已创建视图的原生指针(每次查, 不缓存) |

- 纯 C、无异常;全部返回 `MomoStatus`。表带 `struct_size` 与 `api_version`, **只在末尾追加**, 已有字段不改不删。
- 指针在进程存续期间一直有效, 可以保存、可以在任意线程使用;引擎关闭后各函数返回 `kMomoErrClosed`。
- **宿主只在 platform 线程调用扩展**, 而且只有两类回调: 平台任务、视图工厂。
- 这张表里没有、也不需要的东西: 回到 JS 线程用 `napi_threadsafe_function`;后台线程用 `napi_async_work` 或自己
  开;读应用资源用 `momo:services` 的 `assetsDir`(门面传给插件)或 `node:fs`;日志用自己的方式;跟随帧节奏由 JS
  驱动(框架的 Ticker)或插件自己定时。

**外部纹理**是"扩展借助引擎绘制能力"的途径: 扩展不直接拿 SkCanvas(Skia 的 C++ ABI 不稳定), 而是在自己的线程
生成像素并推给纹理, JS 侧用 `Texture` 视图(或 `SceneBuilder.addTexture` / `Canvas.drawTexture`)按 id 显示,
引擎在 LayerTree 中以 TextureLayer 合成。目前只有 CPU 像素帧。

**平台视图**用于"本身就是系统视图、需要原生输入 / IME / 无障碍"的内容(WebView、地图、系统控件)。扩展只负责
创建与释放视图;嵌入的难点(分片合成、同帧呈现、命中遮挡、焦点交接)全部在引擎与 embedder 里(docs/05 §9)。
JS 侧用框架的 `PlatformView`, 经引擎的 `momo/platform_views` 通道请求创建/销毁。目前只有 macOS 的 Metal surface
支持;软件渲染不支持, Windows embedder 未实现。create 返回的原生视图归扩展所有(macOS 惯用
`CFBridgingRetain` / `CFBridgingRelease`), 每次成功的 create 恰好对应一次 dispose。

## 4. 关闭

扩展的代码随 JS 运行时一起结束(Node 在环境销毁时运行插件登记的清理函数), 所以引擎对扩展的调用必须先结束。
`Shell::Shutdown`(platform 线程)的顺序固定:

```
① 关闭宿主能力表: 此后每个函数返回 kMomoErrClosed;已投递而未执行的平台任务被丢弃(不会执行)
② dispose 全部平台视图(先移出窗口, 再调工厂的 dispose), 清空工厂            ← 此后引擎不再调用任何扩展代码
③ 销毁 JS 运行时: 扩展用 napi_add_env_cleanup_hook 登记的清理函数在这里执行
④ raster: 释放 GPU 资源
```

由此得到扩展作者要记住的两条:

- **清理函数执行时, 宿主已不会再调用你**(视图已全部 dispose, 平台任务不再执行)。在清理函数里停掉自己的线程、
  关掉 `napi_threadsafe_function`。
- ② 时 JS 还在运行, 可能对已经 dispose 的视图 id 发命令: 所以按 id 查视图, 查不到就报错, 不缓存指针。
- ① 丢弃的平台任务不会调用 `fn`, 它的 `user_data` 也就不被释放。关闭只发生在进程退出之前, 这部分内存随进程
  回收;但不要让 `user_data` 持有必须显式归还的东西(文件锁、要落盘的数据)。

一个进程里只有一个 JS 运行时(没有热重启, docs/04 §8), 所以扩展里"属于 JS"的状态与进程同寿, 没有别的生命周期
事件。系统框架自己的在途回调(如 WebKit 的 completion handler)可能晚于 ③ 才到: 扩展要自己保证此时不再碰 JS
(webview 示例: 清理函数里把 threadsafe function 置空, 之后的消息丢弃)。

## 5. SDK 与构建

SDK = 编译一个能被 Momo 加载的插件所需的全部。构建 Momo 时放到 `out/<build>/sdk/`, 也可
`cmake --install <build> --prefix <dir> --component sdk`;发布产物是 `momo-ext-sdk-v<version>-<platform>.zip`。

| 路径 | 内容 |
|---|---|
| `include/momo/momo_host.h` | 宿主能力表(§3) |
| `include/node/` | **Momo 运行时所带 Node 的头文件**(`node_api.h` 等;同 Electron 发布自己的头文件)。不含 openssl |
| `Release/node.lib` | 仅 Windows: `libnode.dll` 的导入库(路径同 node-gyp 的 `--nodedir` 约定) |
| `lib/cmake/MomoExtSdk/` | `find_package(MomoExtSdk)`: `momo::ext_sdk` 与 `momo_add_extension()` |

```cmake
find_package(MomoExtSdk 1 REQUIRED)        # -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk
momo_add_extension(ext_acme src/acme.cc)   # CMakeLists.txt 在 <包>/native/ → 产物 <包>/build/ext_acme.node
```

- **符号从哪来**: Node-API 的符号在 libnode 里, 不在应用的可执行文件里。macOS 上插件以
  `-undefined dynamic_lookup` 链接, 加载时从进程解析。Windows 上插件直接链接 SDK 里的导入库, 导入表指向
  `libnode.dll`(与应用 exe 叫什么无关), 所以不需要 node-gyp 的 delay-load 钩子。
- **用 node-gyp 的扩展**: `node-gyp rebuild --nodedir=<sdk>`(SDK 的布局就是 `--nodedir` 要的布局)。未验证。
- **npm 上预编译的插件**: macOS 上按设计直接可用——符号在加载时从进程解析, 与自己编译的插件走同一条路(自己编译
  的已实测;没有拿 npm 上的预编译包测过)。Windows 上, 用新版 node-gyp 编出的插件带有先找
  `libnode.dll` 的 delay-load 钩子, 按设计可以加载;自己在进程 exe 里查 Node-API 符号的插件加载不了(需要 `Momo.exe`
  转发导出这些符号, 未做)。Windows 路径均未运行验证。
- Momo 用的是未打补丁、默认配置的 Node(版本见 docs/09 §2.1), 所以 Node-API 与 Node 自身的 ABI 都和同版本的官方
  Node 一致;只用 Node-API 的插件不随 Node 版本变化。

## 6. 示例与测试

| 位置 | 内容 |
|---|---|
| `extensions/examples/hello-extension` | 纯 C, 只用 Node-API(不需要宿主表): 同步函数、字符串/字节、线程池上的 Promise |
| `extensions/examples/texture-extension` | C++: 外部纹理, 自己的线程按约 60 fps 推帧;清理函数里停线程 |
| `extensions/examples/webview-extension` | ObjC++(macOS): WKWebView 平台视图;命令经 `post_platform_task` 到主线程, 事件经 `napi_threadsafe_function` 回 JS。示例应用 `examples/webview`(多标签浏览器) |
| `extensions/testing/lifecycle-addon` | 只供测试: 记录宿主对它的每次调用, 验证 §4 的关闭顺序 |

- `engine/extension/extension_unittests.cc`: 宿主能力表的单元测试(只经 C 函数表调用)。
- `extensions/examples/examples_unittests.cc`: 端到端——真实的引擎(Shell + Node 运行时)执行 JS, JS import 真实的 `.node`。
- CTest `ExtensionSdk.StandaloneCxxBuild`: 只凭 `<build>/sdk` 独立配置并编译 texture 示例。
- 工具: `momo ext create <dir>` 生成扩展骨架(`framework/packages/cli/momo.mjs`)。

## 7. 为什么不是自有的扩展 ABI

早期版本有一套自有的扩展机制(`momo_ext_init` 入口、manifest 声明式加载、`ext:*` 模块、自有的 JS 调用参数表、
Promise token、扩展消息通道)。接入 Node.js 之后它的一大半与 Node-API 重复, 而且弱得多(参数只有
number / bool / string / bytes), 还挡住了 npm 上现成的原生包;"静态声明式加载更安全"的理由也不再成立(JS 能
`process.dlopen`)。于是删掉与 Node-API 重复的部分, 只留下引擎能力这一小块, 就是 §3 的那张表。
