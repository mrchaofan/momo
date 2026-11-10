# 04 · JS 运行时与绑定(V8)

本文定义 `engine/runtime/` 的设计:V8 的集成方式、JS↔C++ 绑定基建、模块系统、消息通道与调试。

## 1. V8 集成

- 每个 `engine::Shell` 一个 `v8::Isolate`,运行在 **ui 线程**,一个主 `v8::Context`。首期一窗一引擎一 isolate;多窗口共享 isolate 留作后续(Context 隔离)。
- 冷启动优化用 **V8 code cache**(非 `--dev` 时缓存模块编译结果)。原计划的自定义 startup snapshot 未实现:ESM 模块不支持完整 snapshot(docs/11 M6)。
- Microtask 策略为 explicit:每个宏任务/帧回调结束后由 `JsRuntime::DrainMicrotasks()` 排空,保证与帧管线的相对顺序可控。
- V8 引入方式见 docs/09 §2.1:当前 macOS 为 Homebrew 预编译的 component 构建(dylib 打包进 `.app/Contents/Frameworks`);pointer compression 开(嵌入方定义 `V8_COMPRESS_POINTERS`、`V8_31BIT_SMIS_ON_64BIT_ARCH`)、sandbox 关。进程级初始化(平台、ICU)在 `runtime/js_runtime_impl.cc` 中只做一次。
- ui 线程由 `base::Thread` 以 8 MiB 栈创建(macOS 次线程默认 512 KiB 不足以容纳 V8 的栈上限)。

**M1 已实现**(`engine/runtime/`):`JsRuntime`(公共头无 V8 类型)、`JsRuntimeImpl`(isolate/context、ESM 主模块与相对路径模块、`momo:*` synthetic module、console、异常转 `Status`(含 JS 栈)、未处理 rejection 日志)、`Wrappable` + `ClassBuilder<T>`(宿主类绑定, 类型校验无需 RTTI, GC 弱回调 + 销毁时兜底释放)、`js_args.h`(参数解码)、`js_tags.h`(V8 指针类型标签分配表)。

## 2. V8 封装的边界

不另做一层通用 handle 封装(早期草案中的 `JsScope/JsValue/JsObject` 没有实现):`v8.h` 只允许出现在 `runtime/` 与 `ui/` 的实现文件里,这些文件直接使用 V8 API;其余模块只看到不含 V8 类型的接口。

- `runtime/js_runtime.h`:`JsRuntime`(公共头,无 V8 类型)——创建/销毁、`RunMainModule(bundle)`、`DrainMicrotasks()`。
- `runtime/js_runtime_impl.h`:`JsRuntimeImpl`——拥有 `v8::Isolate` 与主 `Context`;`Call(fn, recv, argv, label)` 把 JS 异常收敛为 `Status`(含 JS 栈),C++ 侧无异常。
- `runtime/js_args.h`:绑定函数的参数解码(`ArgNumber`、`ArgWrapped<T>`、`ToUtf8`…),类型不符时抛 JS `TypeError`。

## 3. 原生绑定基建

**模块**:`JsRuntimeImpl::RegisterNativeModule(specifier, factory)` 注册一个 synthetic ESM 模块(如 `momo:ui`),`factory(isolate, context, exports)` 在模块首次被 import 时填充导出。`momo:ui` / `momo:services` 由 `ui::UiBindings::Install` 注册,`ext:*` 由 `ui/extension_bindings.cc` 注册。

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

- 应用代码与框架层均为 **ESM**;`JsRuntimeImpl::ResolveModule` 实现 V8 的 module resolve 回调,支持相对/绝对路径与 `momo:*`、`ext:*`。
- 内置 synthetic module:`momo:ui`、`momo:services`。
- 扩展 JS 包以 `ext:<name>` specifier 暴露(如 `ext:sqlite`),其 JS 部分打进应用 bundle,原生部分由 ExtensionHost 注册(docs/07)。
- 生产模式:单 bundle(esbuild 产物)+ V8 code cache;startup snapshot 尚未实现(docs/11 M6)。

## 5. 消息通道(MessageChannelHub)

异步、面向字节的双向通道,对标 Flutter BinaryMessenger:

```
JS:  services.send(channel: string, bytes: ArrayBuffer) -> Promise<ArrayBuffer>
     services.setHandler(channel, (bytes) => Promise<ArrayBuffer>)
C++: hub.SetHandler(channel, handler(bytes, reply_cb))   // 任意线程注册, 回调约定线程
     hub.Send(channel, bytes, reply_cb)                   // native -> JS
```

其上提供 `StandardCodec`(与 Flutter StandardMessageCodec 同思路的二进制编码:null/bool/int/double/string/bytes/list/map),JS 与 C++ 各一份实现。平台服务(docs/03 §5)与扩展消息(docs/07)复用之。

## 6. 事件入口与帧回调

`momo:ui` 的 window 对象上暴露(由 `ui::EventDispatcher` 调用):

```ts
window.onBeginFrame: (frameTimeMs: number) => void      // 动画推进 + build/layout/paint(单调时钟毫秒)
window.onDrawFrame: () => void                          // 提交 scene
window.onPointerPacket: (packet: PointerDataView) => void
window.onKeyData: (data: KeyDataView) => boolean        // 返回 true 表示已消费
window.onMetricsChanged: () => void                     // 读 window.physicalSize / devicePixelRatio
window.onCloseRequested: () => boolean | void           // 返回 false 取消关闭
window.scheduleFrame(): void
window.render(scene: Scene): void
```

事件对象使用预分配的 TypedArray 视图复用,避免每事件分配。

已全部实现(含 M2 的 onPointerPacket(Float64Array 平铺, 缓冲复用)/onKeyData/onFocusChanged、setCursor、setMinSize,
M3 的 Paragraph/Image/Codec/loadAsset, M5 的 drawTexture/addTexture);完整 TS 声明见
`framework/packages/momo/src/ui/momo-ui.d.ts`。消息通道实现: `engine/common/message_channel_hub.h`(回复总是投递,
不在 JS 调用栈内同步重入)、`engine/common/standard_codec.h`、JS 端 `momo:services`(`engine/ui/services_bindings.cc`)。
内置通道 momo/lifecycle、momo/clipboard、momo/url、momo/textinput、momo/extensions。

## 7. 调试与诊断

- `--dev` 启动时 `runtime::Inspector` 监听 127.0.0.1 上的 WebSocket(默认 9229, `--inspect-port=`;`--inspect-brk`
  执行 bundle 前等待调试器),提供 `/json/list`、`/json/version` 发现端点, Chrome DevTools 直连:断点、console、
  profile。断点暂停时 ui 线程阻塞在 Inspector 自己的消息队列(base/blocking_queue.h)上继续处理 DevTools 消息。
- `console.*` 绑定到引擎日志(spdlog),带 JS 调用栈。
- 未捕获异常/未处理 rejection:开发模式红屏(engine 绘制错误层)+ 控制台;生产模式回调 `onError` 通道交由应用上报。

## 8. 热重载(开发体验,M4+)

已实现 hot restart: `--dev` 下 embedder 每 500ms 检查 bundle 修改时间, 变化时 `Shell::RequestHotRestart` →
ui 线程销毁 JS 运行时并重新执行 bundle(新 bundle 出错时退回占位驱动, 修复后可再次重启)。

监听 bundle 文件变化 → 重建 Context(保留 Isolate)→ 重新执行 bundle → 框架层从根重建树。状态保持(类似 Flutter hot reload)不做首期承诺,先做秒级 hot restart。
