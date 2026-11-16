# 04 · JS 运行时与绑定(V8 / Node.js)

本文定义 `engine/runtime/` 的设计:V8 的集成方式、JS↔C++ 绑定基建、模块系统、消息通道与调试。

## 1. V8 与 Node.js 集成

- 每个 `engine::Shell` 一个 **Node.js 环境**(Node 26 LTS 的 libnode,docs/09 §2.1):`node::CommonEnvironmentSetup`
  = libuv 循环 + `v8::Isolate` + IsolateData + `node::Environment`,运行在 **ui 线程**,主 Context 即 Node 的主 Context。
  JS 可以使用 `node:*` 内置模块、npm 包(node_modules)、Node 的定时器与 IO。首期一窗一引擎一 isolate。
- 进程级初始化 `node::InitializeOncePerProcess` 只做一次(V8 平台、V8、ICU、OpenSSL):不改动 stdio/TTY、不安装
  Node 的信号处理、不读 `NODE_OPTIONS`。V8 平台即 Node 的 `MultiIsolatePlatform`(后台任务在它的线程池上)。
  ui 线程整个运行时生命周期持有 `v8::Locker` 并保持 isolate 进入状态。
- **libuv 接到 ui 线程**(`runtime/node_bindings.{h,cc}`,对照 Electron `shell/common/node_bindings`):
  ```
  ui 线程(V8 + momo EventLoop)                 poll 线程 "momo.uv.poll"(uv_thread)
  StartPolling() → UvRunOnce()                   loop: uv_sem_wait(&embed_sem_)    ← 等 ui 跑完一轮
  UvRunOnce(): HandleScope + Context::Scope            PollEvents(): poll(uv_backend_fd,
               uv_run(loop, UV_RUN_NOWAIT)                           uv_backend_timeout)  只等待
               uv_sem_post(&embed_sem_)  ──────────►   WakeupMainThread(): 投递 UvRunOnce 到 ui
  ```
  uv 回调(定时器、IO、V8 前台任务)都在 ui 线程执行,poll 线程只等内核事件;`uv_sem` 保证二者严格交替。
  `uv_async` 空句柄保持循环存活(否则 poll 线程空转)并用于唤醒。与 Electron 的差异:Electron 给 libuv 打了
  `uv_loop_interrupt` 补丁,让 ui 线程上新增的定时器/IO 打断 poll 线程;momo 用上游 libuv,改为每次从 momo 进入 JS
  之后(`DrainMicrotasks`)`uv_async_send` 唤醒 poll 线程(自带合并)。
- **入口**:`node::LoadEnvironment` 的 preload 中运行 `runtime/node_bootstrap.cc`(可访问 Node 内部模块),它安装模块钩子
  并返回宿主对象;`RunMainModule` 经 Node 的 ESM 加载器(cascaded loader)导入 bundle,并在 ui 线程 **以 `uv_run(UV_RUN_ONCE)`
  泵送**直到入口开始求值(同 Electron `JoinAppCode`):模块图的文件读取在这里完成, 入口的同步部分随即执行完。
  **入口不等待顶层 await**(与 Electron 一致):入口停在顶层 await 时 `RunMainModule` 立即返回, 余下的求值在正常的事件
  循环里继续——泵送的只有 uv, 等下去的话 ui 线程上的引擎任务(平台通道应答、帧)无法执行, 启动会卡住。因此入口里
  应当同步调用 `runApp`, 不要把它放在顶层 await 之后。加载错误与同步部分的求值错误以
  Promise 拒绝返回宿主,转成 `Status`(含 JS 栈);顶层 await 之后的错误只记录日志。
- **从 momo 进入 JS**(帧回调、输入、通道回复、图片解码):`JsRuntime::Call` 在 `node::CallbackScope`
  里调用(同 `node::MakeCallback`),最外层的调用返回前按 Node 语义排空 `process.nextTick` 与 microtask(nextTick 先于
  Promise)。入口都是 ui 线程上的任务,与 uv 回调同属一个循环,顺序即到达顺序。之后 `DrainMicrotasks()` 唤醒 poll 线程。
- **错误**:C++ 调用 JS 的异常收敛为 `Status` 并记日志;uv 回调里的未捕获异常与未处理的 Promise 拒绝由 bootstrap 注册的
  `process.on('uncaughtException' / 'unhandledRejection')` 打印到 stderr,**不退出进程**(Node 默认会退出)。
  `process.exit()` 保持 Node 默认行为(退出进程)。
- 冷启动优化用 Node 的**编译缓存**(`module.enableCompileCache`,非 `--dev` 时启用,入口执行后 flush)。startup snapshot 未实现。
- V8 配置跟随 Node 默认:pointer compression 关、sandbox 关;ICU 数据编入 libnode。
- ui 线程由 `base::Thread` 以 8 MiB 栈创建(macOS 次线程默认 512 KiB 不足以容纳 V8 的栈上限);poll 线程 256 KiB。
- 关闭:停 poll 线程(`embed_closed_` + 信号量 + 唤醒 + join)→ 关闭保活句柄 → 退出 isolate、释放 Locker →
  `CommonEnvironmentSetup` 析构(FreeEnvironment 运行循环直到句柄关闭、销毁 isolate、关闭 uv 循环)。一个进程里只有一个运行时: `Shell::Run` 创建, `Shell::Shutdown` 销毁。

**已实现**(`engine/runtime/`):`JsRuntime`(Node 环境、ESM 入口、`momo:*` 原生模块、异常转 `Status`(含 JS 栈);头文件含 V8 类型, 只被 runtime 与 ui 的实现文件包含——引擎的其余部分经不含 V8 的 `ui::AppDriver` 驱动 JS)、`NodeBindings`(libuv ↔ ui 线程)、`Wrappable` + `ClassBuilder<T>`(宿主类绑定, 类型校验无需 RTTI, GC 弱回调 + 销毁时兜底释放)、`js_args.h`(参数解码)、`js_tags.h`(V8 指针类型标签分配表)。

## 2. V8 封装的边界

不另做一层通用 handle 封装(早期草案中的 `JsScope/JsValue/JsObject` 没有实现):`v8.h` 只允许出现在 `runtime/` 与 `ui/` 的实现文件里,这些文件直接使用 V8 API;其余模块只看到不含 V8 类型的接口。

- `runtime/js_runtime.h`:`JsRuntime`——拥有 Node 环境(`v8::Isolate` 与主 `Context`):创建/销毁、`RunMainModule(bundle)`、`DrainMicrotasks()`;`Call(fn, recv, argv, label)` 把 JS 异常收敛为 `Status`(含 JS 栈),C++ 侧无异常。
- `runtime/js_args.h`:绑定函数的参数解码(`ArgNumber`、`ArgWrapped<T>`、`ToUtf8`…),类型不符时抛 JS `TypeError`。

## 3. 原生绑定基建

**模块**:`JsRuntime::RegisterNativeModule(specifier, factory)` 注册一个 synthetic ESM 模块(如 `momo:ui`),`factory(isolate, context, exports)` 在模块首次被 import 时填充导出。`momo:ui` / `momo:services` 由 `ui::UiBindings::Install` 注册。`momo:services` 除消息通道外还导出 `assetsDir`(应用资源目录的绝对路径)与 `nativeHost`(BigInt: 宿主能力表的地址, 原生扩展用, docs/07 §3)。

**宿主类**:C++ 对象继承 `Wrappable`,用 `ClassBuilder<T>` 暴露为 JS 类:

```cpp
ClassBuilder<JsPaint>(isolate, "Paint", &JsPaint::Construct)   // ctor 传空 = JS 不能 new
    .Property("color", &GetColor, &SetColor)
    .Method("copy", &Copy)
    .Export(context, exports);
```

约定:

- JS 对象的 internal field 指向 `Wrappable`;类型校验靠 `WrapperTypeInfo`(无 RTTI)。生命周期:JS 包装对象被 GC 时(弱回调)删除 C++ 对象,运行时销毁时删除仍存活的全部对象;大对象(`Image`、`Codec`、`Paragraph`)另提供显式 `dispose()`,图片向 isolate 报告外部内存。
- 高频小数据走 TypedArray 平铺传参(如指针包:每个指针 10 个 double 写入复用的 `Float64Array`),避免逐属性读取对象。
- 绑定函数内部不得阻塞、不得调用平台 API(必要时投递到 platform/io 线程并返回 Promise)。

## 4. 模块系统

- 应用代码与框架层均为 **ESM**,由 Node 的 ESM 加载器执行:相对/绝对路径、`node:*`、npm 包(按 Node 规则从 node_modules
  解析)。momo 应用自己的 `.js` 文件一律按 ESM 处理(不依赖 package.json 的 `type`);node_modules 中的包保持 Node 的格式判定。
- `momo:*` 一律是**运行时提供的模块**, 由 bootstrap(`runtime/node_bootstrap.cc`)的 `module.registerHooks` 解析, 有两种来源:
  - **运行时自带的 JS 模块**(框架 `momo:framework`、内置扩展 `momo:webview`):`<builtin_dir>/<name>/package.json` 存在, 就解析到
    它的 `main`;之后它是一个普通的文件模块, 相对导入与旁边的 `.node` 照常解析。`builtin_dir` 是 `Settings` 的一项
    (`engine/common/settings.h`):embedder 设为 `<resources>/builtin`, `momo_headless` 用可执行文件同目录的 `builtin/`;空 = 没有。
  - **引擎的原生模块**(`momo:ui`、`momo:services`):不是上面那种就走这里。`RegisterNativeModule(specifier, factory)` 以
    `node::AddLinkedBinding` 注册原生绑定(import 时才调用 factory 填充导出), 钩子把它解析为 ESM 模块, 内容从
    `process._linkedBinding(specifier)` 读出导出。

  谁 import 都是同一份:内置扩展里的 `import "momo:framework"` 与应用拿到的是同一个模块实例。应用的产物里 `momo:*` 的导入
  原样保留(框架不进应用的代码), 所以框架版本 = 运行时版本, `momo:ui` 是框架与引擎之间的内部接口。
- 原生扩展是 Node 原生插件(docs/07):Node 自己只在 `require` 里认 `.node`, 同一个模块钩子让
  `import addon from "./x.node"` 也能用(默认导出是插件的 exports)。内置扩展的插件在 `<builtin_dir>/<name>/` 里, 与它的 JS
  同目录;三方的带原生插件的包保持原样, 按 Node 的规则从应用的 `node_modules` 加载, 包内的 JS 按相对路径找到自己的
  `.node`(docs/07 §2)。
- `RunModuleSource(source, name, base_dir)`(测试与 `--dev` 片段):源码由加载钩子提供,url 带递增查询串(每次都是新模块),
  相对导入基于 `base_dir`。
- 生产模式:Node 编译缓存(应用的入口是否打成单文件由应用自己决定, docs/09 §4);startup snapshot 尚未实现(docs/11 M6)。

## 5. 消息通道(MessageChannelHub)

异步、面向字节的双向通道,对标 Flutter BinaryMessenger:

```
JS:  services.send(channel: string, bytes: ArrayBuffer) -> Promise<ArrayBuffer>
     services.setHandler(channel, (bytes) => Promise<ArrayBuffer>)
C++: hub.SetHandler(channel, handler(bytes, reply_cb))   // 任意线程注册, 回调约定线程
     hub.Send(channel, bytes, reply_cb)                   // native -> JS
```

其上提供 `StandardCodec`(与 Flutter StandardMessageCodec 同思路的二进制编码:null/bool/int/double/string/bytes/list/map),JS 与 C++ 各一份实现。平台服务(docs/03 §5)复用之。

## 6. 事件入口与帧回调

`momo:ui` 的全局 `window` 与每个视图(`views.get(id)`)上暴露(由 `UiBindings` 调用):

```ts
window.onBeginFrame: (frameTimeMs: number) => void      // 动画推进 + build/layout/paint(单调时钟毫秒)
window.onDrawFrame: () => void                          // 各视图提交 scene
window.onViewsChanged: () => void                       // 宿主打开/关闭了窗口
window.scheduleFrame(): void
view.onPointerPacket: (data: Float64Array, count: number) => void
view.onKeyData: (data: KeyData) => boolean              // 返回 true 表示已消费
view.onMetricsChanged: () => void                       // 读 view.physicalSize / devicePixelRatio
view.onFocusChanged: (focused: boolean) => void
view.render(scene: Scene): void
```

应用生命周期与窗口的标题、光标、位置、关闭询问等不在 `momo:ui` 里: 它们属于宿主, 经宿主注册的 `momo/app` / `momo/window` 通道(框架的 `app` / `Widget`)。`process.exit(code)` 不直接退出进程: 运行时用 `node::SetProcessExitHandler` 停止 JS 并把退出码交给宿主有序关闭(ARCHITECTURE §3.8)。

事件对象使用预分配的 TypedArray 视图复用,避免每事件分配。

已全部实现(含 M2 的 onPointerPacket(Float64Array 平铺, 缓冲复用)/onKeyData/onFocusChanged、setCursor、setMinSize,
M3 的 Paragraph/Image/Codec/loadAsset, M5 的 addTexture);完整 TS 声明见
`framework/packages/momo/src/ui/momo-ui.d.ts`。消息通道实现: `engine/common/message_channel_hub.h`(回复总是投递,
不在 JS 调用栈内同步重入)、`engine/common/standard_codec.h`、JS 端 `momo:services`(`engine/ui/services_bindings.cc`)。
内置通道 momo/app、momo/window、momo/clipboard、momo/url、momo/textinput、momo/platform_views。
JS 运行时销毁时移除 JS 端点, 之后发往 JS 的消息(含在途的)回复空;已投递给 native handler 的消息由 handler 的注册方自己判活。

## 7. 调试与诊断

- `--dev` 启动时启用 **Node 的 inspector**(`--inspect=127.0.0.1:<port>`,默认 9229,`--inspect-port=` 修改;`--inspect-brk`
  映射为 Node 的 `--inspect-wait`:执行 bundle 前等待调试器连接, 期间 ui 线程阻塞, 主线程照常泵消息。此时退出应用,
  销毁要等 ui 线程——即等到调试器连上并断开——才完成, 不强行打断)。Chrome DevTools / VS Code 直连:断点、console、profile、
  heap snapshot。inspector 选项是进程级的。
- `console.*` 是 Node 的 console(写 stdout/stderr,连着 inspector 时同时送到 DevTools)。
- 未捕获异常/未处理 rejection:打印到 stderr 并继续运行(见 §1)。

## 8. 热重启: 不支持

一个进程里只有一个 JS 运行时。曾经有过实验性的热重启(bundle 变化时销毁运行时、新建一个重新执行), 已删除:
重置 JS 而不重置宿主, 意味着每个 JS 模块、每个扩展都要能正确撤销自己留在宿主里的状态(窗口、平台视图、在途的通道
请求、按 id 登记的对象), 这类错误只在热重启时出现, 测不到;而最需要重启的场景(顶层抛异常、死循环)恰恰没有机会
执行任何撤销逻辑。开发循环是重新打 bundle 后重启进程。随之消失的还有: 运行时的代数、通道消息的代际、扩展的
生命周期监听(`RuntimeStarting` / `RuntimeStopped`)——扩展里属于 JS 的状态与进程同寿(docs/07 §4)。
