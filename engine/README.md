# engine/ — 核心引擎层(C++)

设计文档:`docs/02-engine.md`(总览)、`docs/12-base.md`(基础库)、`docs/04-js-runtime.md`、`docs/05-rendering-pipeline.md`、`docs/08-threading-model.md`。

| 模块 | 库名 | 职责 | 关键文件 |
|---|---|---|---|
| `platform/` | momo_engine_platform(INTERFACE) | 平台契约: engine 对宿主的全部要求(纯接口 + POD), 由 embedder 实现 | `render_surface.h`、`vsync_source.h`、`input_types.h`、`text_input_connection.h`、`platform_services.h`、`system_cursor.h`、`viewport_metrics.h` |
| `common/` | momo_engine_common | 无 V8/Skia 的公共设施: 启动设置、TaskRunners、消息通道、StandardCodec、JSON | `settings.h`、`task_runners.h`、`message_channel_hub.h/.cc`、`standard_codec.h/.cc`、`json.h/.cc` |
| `shell/` | momo_engine_shell | Shell、ThreadHost、Animator、Rasterizer、RenderBackend(软件/Metal)、Engine 内部类、平台服务通道 | `shell.h/.cc`、`thread_host.h/.cc`、`animator.h/.cc`、`rasterizer.h/.cc`、`render_backend.h/.cc`、`engine.h/.cc` |
| `runtime/` | momo_engine_runtime | Node.js 环境(libnode)、libuv 接入 ui 线程、ESM 入口与 `momo:*` 模块、绑定基建 | `js_runtime.h/.cc`、`js_runtime_impl.h/.cc`、`node_bindings.h/.cc`、`node_bindings_posix.cc`、`node_bootstrap.h/.cc`、`class_builder.h`、`wrappable.h/.cc` |
| `ui/` | momo_engine_ui | momo:ui / momo:services / ext:* 绑定、事件入口 | `ui_bindings.h`、`ui_impl.h`、`window_binding.cc`、`painting_bindings.cc`、`scene_bindings.cc`、`text_bindings.cc`、`image_bindings.cc`、`services_bindings.cc`、`extension_bindings.cc`、`app_driver.h`、`js_app_driver.cc`、`app_driver_no_js.cc` |
| `graphics/` | momo_engine_graphics | DisplayList、LayerTree、Image、外部纹理、软件光栅与 Metal 后端 | `display_list.h/.cc`、`layer.h/.cc`、`image.h`、`texture.h/.cc`、`software_rasterizer.h/.cc`、`gpu/metal_rasterizer.h/.mm`、内部 `skia/` |
| `text/` | momo_engine_text | 字体集合(系统字体 + 内嵌字体 + 回退)与段落排版(SkParagraph) | `font_collection.h/.cc`、`paragraph.h/.cc` |
| `assets/` | momo_engine_assets | 资源读取(路径越界防护)、图片解码(静态/多帧) | `asset_manager.h/.cc`、`image_decoder.h/.cc` |
| `extension/` | momo_engine_extension | 扩展宿主: manifest、dlopen、版本握手、MomoHostApi(C ABI 实现侧) | `extension_host.h/.cc` |

硬性规则:v8.h 不出 runtime/ui 实现文件;Skia 头不出 graphics/text/assets;engine 只认识自己的平台契约 `engine/platform/`,禁止 include `embedder/`。

## 状态(M5)

全部模块已实现。图片缓存(ImageCache, LRU)在框架层 `painting/`(与 Flutter 一致), 引擎只负责解码。
GPU: Metal 后端已接入(创建失败自动回退软件);ANGLE/D3D(Windows)未实现。

帧路径:JS `window.scheduleFrame()` → `Animator::RequestFrame` → VsyncWaiter → `Engine::BeginFrame`(JS onBeginFrame + onDrawFrame → `window.render(scene)`)→ `Animator::Render` → `Pipeline`(深度 2)→ raster `Rasterizer::Draw` → `graphics::RasterizeLayerTree` → surface present。

依赖规则落实:Skia 头只在 `graphics/` 实现文件(及 `graphics/skia/` 内部头);`v8.h` 只在 `runtime/`、`ui/` 的实现文件与内部头;shell 只见无 V8 的 `ui/app_driver.h`。`ui::CreateAppDriver` 的实现由 ui 模块的构建配置决定:有 V8 为 `ui/js_app_driver.cc`;sanitizer 构建没有 V8, 不构建 runtime, ui 只含 `ui/app_driver_no_js.cc`(占位帧)。shell 总是链接 ui, 模块间无环。生产代码中没有 `#if` 分支。

CMake target:`momo_engine_platform`、`momo_engine_common`、`momo_engine_graphics`、`momo_engine_text`、`momo_engine_assets`、`momo_engine_extension`、`momo_engine_runtime`、`momo_engine_ui`(runtime 仅在有 V8 时)、`momo_engine_shell`、聚合 `momo::engine`。单测:`momo_engine_unittests`(测试替身在 `shell/testing/shell_test_utils.h`)。

**新模块接入约定**:链接 `momo_base`(需要平台接口时再加 `momo_engine_platform`);有线程归属的类继承 `momo::Object`;以仓库根为 include 根(`#include "base/status.h"`);禁止 include `embedder/`。
