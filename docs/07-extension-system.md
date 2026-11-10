# 07 · 扩展机制设计

一方暴露的 JS API 永远不够(数据库、音视频、托盘、硬件、现有 C/C++ SDK…)。Momo 的扩展机制允许三方开发者**用原生代码直接调用平台能力,并借助 engine 层能力(JS 绑定、消息通道、窗口句柄、GPU 上下文、帧回调)把功能安全地暴露给 JS**。对标 Flutter Plugin + RN TurboModule,但以**稳定 C ABI** 为边界(扩展与宿主可用不同编译器/标准库版本构建)。

## 1. 扩展的形态

一个扩展 = **JS 包 + 可选原生库**:

```
ext-sqlite/
├── package.json          # momo 字段声明原生库与权限
├── js/index.ts           # JS 门面: 封装消息通道/原生函数, 提供好用的 API
├── native/
│   ├── CMakeLists.txt
│   └── src/ext_sqlite.cc # 实现 momo_ext_init 入口
└── prebuilt/             # 分发时附带: windows-x64/ext_sqlite.dll, macos-arm64/....dylib
```

`package.json` 声明:

```json
{
  "name": "@acme/ext-sqlite",
  "momo": {
    "extension": {
      "id": "acme.sqlite",
      "native": { "windows-x64": "prebuilt/windows-x64/ext_sqlite.dll",
                   "macos-arm64": "prebuilt/macos-arm64/libext_sqlite.dylib" },
      "specifier": "ext:sqlite",
      "capabilities": ["filesystem"]
    }
  }
}
```

构建工具(tools/momo-cli)收集依赖中的扩展声明,生成 `extensions.manifest`(打进应用资源),并把原生库拷入应用包。**加载是静态声明式的**——不支持运行期从任意路径 dlopen,降低被当作通用注入器的风险。

## 2. C ABI(extensions/sdk/include/momo/momo_extension.h)

C++ 扩展可使用其上的 header-only 封装 `momo_extension.hpp`(不引入新的跨边界符号);SDK 以 CMake 包 `MomoExtSdk` 随构建产物发布(`out/<build>/sdk/`),见 extensions/README.md。

扩展只依赖这一个头文件。核心形状:

```c
#define MOMO_EXT_API_VERSION 1

typedef struct MomoHostApi MomoHostApi;   // 宿主函数表(见 §3)
typedef struct MomoExtension {
  uint32_t struct_size;
  const char* id;                          // "acme.sqlite"
  void* user_data;
  void (*on_attach)(void* user_data, const MomoHostApi* host, MomoEngineRef engine);
  void (*on_detach)(void* user_data);      // 引擎关闭前, 必须释放一切宿主资源
} MomoExtension;

// 扩展导出的唯一入口(C 符号):
MOMO_EXT_EXPORT bool momo_ext_init(uint32_t host_api_version, MomoExtension* out);
```

版本策略:`MomoHostApi` 只增不改不删(结构体带 size 字段);扩展在 init 中检查版本号,宿主拒绝过新的扩展并给出明确错误。

## 3. 宿主能力面(MomoHostApi)

按能力域分组的函数表,全部 C 签名、无异常传播(返回 `MomoStatus`):

| 能力域 | 代表函数 | 说明/线程约定 |
|---|---|---|
| 模块注册 | `register_module(engine, "ext:sqlite", setup_cb)` | setup 回调在 ui 线程被调,用 `MomoJsBuilder` 声明函数 |
| JS 函数 | `MomoJsBuilder.add_function(name, fn, user_data)` | fn 收 `MomoCallInfo`(读参数/设返回值/抛 JS 错误的 C API),**在 ui 线程被调,禁止阻塞** |
| 消息通道 | `channel_set_handler / channel_send / channel_reply` | 字节流;handler 可声明回调线程(platform/pool) |
| 任务投递 | `post_task(engine, runner_kind, fn, user_data)`、`post_delayed` | runner_kind ∈ {platform, ui, io, worker_pool} |
| 窗口访问 | `get_window_handle(view_id)` → HWND/NSView* | 仅 platform 线程;托盘、子窗口、系统集成用 |
| 帧参与 | `add_frame_callback(engine, cb)` | ui 线程,vsync 后调用;视频/图表类扩展驱动动画 |
| 纹理注入 | `texture_registry_register(...)`, `texture_mark_frame_available` | 扩展在自己线程生成像素/GPU 纹理,引擎合成时采样——视频播放、相机、原生渲染互操作的正路 |
| 平台视图 | `platform_view_register_factory(engine, "acme.webview", factory)`、`platform_view_get(id)` | 仅 platform 线程。扩展提供原生视图(NSView*/HWND)的 create/dispose,引擎负责嵌入、合成、输入与焦点(docs/05 §9)——WebView、地图、系统控件的正路 |
| 资源 | `open_asset(path)` | 读取打进应用包的扩展资源 |
| 日志/崩溃 | `log(level, tag, msg)` | 汇入引擎日志 |

**平台视图与纹理注入互补**:纹理适合"扩展自己生成像素"的内容(视频帧、相机);平台视图适合"本身就是系统视图、
需要原生输入/IME/无障碍"的内容(WebView)。扩展只负责创建与释放视图,嵌入的难点(分片合成、同帧呈现、命中遮挡、
焦点交接)全部在引擎与 embedder 中解决, 因此三方扩展无需接触引擎内部 API。

**纹理注入是"扩展借助 engine 绘制能力"的核心设计**:扩展不直接拿 SkCanvas(Skia C++ ABI 不稳定),而是注册外部纹理(IOSurface/D3D 共享句柄/CPU 位图),JS 侧用 `<Texture textureId>` widget 占位,引擎在 LayerTree 中以 TextureLayer 合成。

## 4. JS 侧形态

JS 门面把底层通路包装成友好 API:

```ts
// ext-sqlite/js/index.ts
import { native } from "ext:sqlite";          // ExtensionHost 注册的 synthetic module
export class Database {
  static async open(path: string): Promise<Database> { ... } // 内部走 native.open / 消息通道
}
```

对应用开发者而言与普通 npm 包无差别:`npm i @acme/ext-sqlite` + `import { Database } from "@acme/ext-sqlite"`。

## 5. 加载与生命周期(engine/extension/)

```
Shell 启动 → ExtensionHost 读 extensions.manifest
 → 逐个 dlopen/LoadLibrary(应用包内路径) → 解析 momo_ext_init → 版本握手
 → on_attach(host_api, engine)            [platform 线程]
 → 扩展注册模块/通道; ui 线程建 JS 绑定
 … 运行期 …
 → 引擎关闭(JS 运行时已销毁): on_detach → 排空并停止扩展 worker pool → 拒绝该扩展此后的一切 API 调用
   (动态库不卸载: 扩展线程上可能仍有在途回调, 与 Flutter 插件一致)
```

失败隔离:单个扩展 init 失败只禁用该扩展并上报(应用可在 JS 捕获 `momo/extensions` 通道的错误),不拖垮引擎。扩展内崩溃无法隔离(同进程),文档明确要求扩展自行保证健壮性;`--dev` 下加载时带符号名日志辅助定位。

## 6. 能力与权限

manifest 中 `capabilities` 字段声明扩展需要的敏感能力(filesystem/network/window/texture/platform-view…)。首期仅做**构建期可见性**(应用开发者在构建产物报告里看到每个扩展申请了什么)+ 运行期日志;真正的 OS 级沙箱不在首期范围。

## 7. 开发者工作流

`tools/momo-cli` 提供:`momo ext create`(脚手架:C ABI 模板 + TS 门面 + CMake 预设)、`momo ext build`(双平台交叉产出 prebuilt/)、`momo ext verify`(ABI 符号检查、manifest 校验)。SDK 头文件与 TS 类型随 `extensions/sdk/` 发布并带语义化版本。

## 8. 实现状态(M5)

- 宿主: `engine/extension/extension_host.{h,cc}`(manifest 解析、dlopen/LoadLibrary、版本与 id 握手、失败隔离、
  MomoHostApi 的非 JS 部分);JS 胶水: `engine/ui/extension_bindings.cc`(`ext:*` synthetic module、MomoCallApi)。
- 线程: `on_attach`/`on_detach` 在 platform 线程;`register_module` 只在 `on_attach` 中有效(JS 运行时创建时安装);
  `channel_set_handler` 可指定 handler 线程(platform/ui/io/worker pool);`promise_*`、`post_task`、`texture_*` 任意线程。
- 纹理: v1 只支持 CPU 像素帧(`texture_update_pixels`, 或 acquire/release 在调用 `texture_mark_frame_available`
  的线程上同步回调);宿主拷贝为不可变 Image, 合成时取最新帧(DisplayList 中含纹理的部分不进入 SkPicture 缓存)。
  GPU 共享句柄(IOSurface / D3D)为预留。
- 引擎保留 `momo/*` 通道名;扩展加载错误经 `momo/extensions` 通道(string)报给 JS。
- 工具链: `tools/momo-cli/momo.mjs` 的 `build`(manifest 生成 + 库拷贝 + 能力报告)、`ext create`、`ext verify`。
- 平台视图(v1 追加, M6): `MomoHostApi` 末尾追加 `platform_view_register_factory` / `platform_view_get`(扩展以
  `struct_size` 判断宿主是否提供;C++ 封装 `Host::SupportsPlatformViews / RegisterPlatformViewFactory / PlatformView`)。
  create 返回的原生视图归扩展所有(macOS 惯用 `CFBridgingRetain`/`CFBridgingRelease`),每次成功 create 恰好对应一次
  dispose,且全部早于 `on_detach`;类型名 `momo/` 前缀保留。能力声明 `platform-view`。
- 示例: `extensions/examples/{hello,texture,webview}-extension`(webview: macOS WKWebView 平台视图 + JS 门面
  `WebView` / `WebViewController`;示例应用 `examples/webview`: 多标签浏览器)(tray 示例已在精简示例时删除, 窗口句柄 API 由 `ExtensionHostTest.CallsAfterDetachAreRejected` 覆盖);端到端测试 `engine/extension/extension_unittests.cc`。
