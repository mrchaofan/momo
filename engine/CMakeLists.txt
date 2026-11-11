# engine/: 每个模块一个静态库 momo_engine_<module>(docs/02 §1)。基础设施(线程/任务/对象/Status…)在顶层 base/。
# 模块依赖(单向, 无环; A → B 表示 A 依赖 B):
#   shell → ui → {runtime, extension, text, assets} → graphics → base
#   extension → common;各模块按需 → platform(纯接口)/ common(Settings、TaskRunners、消息通道)

if(NOT MOMO_HAVE_SKIA)
  message(FATAL_ERROR "engine requires the Skia prebuilt: python3 tools/fetch_deps.py skia")
endif()

# ---- momo_engine_platform: engine 对宿主(embedder)的平台契约, 纯接口 + POD(docs/03) ----
# RenderSurface、VsyncSource、输入事件、IME、PlatformServices、ViewportMetrics、SystemCursor。
# engine 只认识这些接口;embedder 实现它们并注入。零平台 include、零三方依赖。
add_library(momo_engine_platform INTERFACE)
target_link_libraries(momo_engine_platform INTERFACE momo_base)

# ---- momo_engine_common: 无 V8/Skia 的引擎公共设施(Settings、TaskRunners、消息通道、StandardCodec、JSON) ----
add_library(momo_engine_common STATIC
  common/json.cc
  common/message_channel_hub.cc
  common/platform_view_registry.cc
  common/standard_codec.cc)
target_link_libraries(momo_engine_common PUBLIC momo_base)

# ---- momo_engine_graphics: DisplayList、Layer、软件光栅(Skia 只在本模块实现文件中出现) ----
add_library(momo_engine_graphics STATIC
  graphics/composition.cc
  graphics/display_list.cc
  graphics/layer.cc
  graphics/path.cc
  graphics/texture.cc
  graphics/skia/image.cc
  graphics/skia/skia_conversions.cc
  graphics/software_rasterizer.cc)
target_link_libraries(momo_engine_graphics PUBLIC momo_base PRIVATE momo::skia)
if(APPLE)
  # Ganesh Metal 后端(docs/05 §4 MetalBackend);Skia target 已带 SK_METAL 与 Metal/QuartzCore 框架。
  target_sources(momo_engine_graphics PRIVATE graphics/gpu/metal_rasterizer.mm)
  target_link_libraries(momo_engine_graphics PRIVATE "-framework Metal" "-framework QuartzCore")
endif()

# ---- momo_engine_text: SkParagraph 封装(FontCollection、Paragraph)----
add_library(momo_engine_text STATIC
  text/font_collection.cc
  text/paragraph.cc)
target_link_libraries(momo_engine_text PUBLIC momo_engine_graphics PRIVATE momo::skia)

# ---- momo_engine_assets: AssetManager、图片解码(Skia codecs)----
add_library(momo_engine_assets STATIC
  assets/asset_manager.cc
  assets/image_decoder.cc)
target_link_libraries(momo_engine_assets PUBLIC momo_engine_graphics PRIVATE momo::skia)

# ---- momo_engine_extension: 扩展宿主(C ABI, manifest, dlopen;不含 V8)----
add_library(momo_engine_extension STATIC
  extension/extension_host.cc)
target_link_libraries(momo_engine_extension PUBLIC momo_base momo_engine_common momo_engine_graphics
                      momo_engine_assets momo_ext_sdk)
target_link_libraries(momo_engine_extension PRIVATE ${CMAKE_DL_LIBS})

# ---- momo_engine_runtime / momo_engine_ui: V8(v8.h 只在这两个模块的实现文件中出现) ----
# ui 提供 AppDriver(engine/ui/app_driver.h)的实现, shell 只面向该接口。
# sanitizer 构建没有 V8(docs/09 §2.1): 不构建 runtime, ui 只含占位驱动(无 JS 的占位帧)。
if(MOMO_HAVE_V8)
  add_library(momo_engine_runtime STATIC
    runtime/js_args.cc
    runtime/js_runtime.cc
    runtime/js_runtime_impl.cc
    runtime/node_bindings.cc
    runtime/node_bindings_posix.cc
    runtime/node_bootstrap.cc
    runtime/wrappable.cc)
  target_link_libraries(momo_engine_runtime PUBLIC momo_base momo::v8)

  add_library(momo_engine_ui STATIC
    ui/extension_bindings.cc
    ui/image_bindings.cc
    ui/js_app_driver.cc
    ui/painting_bindings.cc
    ui/scene_bindings.cc
    ui/services_bindings.cc
    ui/text_bindings.cc
    ui/window_binding.cc)
  target_link_libraries(momo_engine_ui PUBLIC momo_engine_runtime momo_engine_common momo_engine_graphics
                        momo_engine_text momo_engine_assets momo_engine_extension
                        momo_engine_platform)
else()
  add_library(momo_engine_ui STATIC ui/app_driver_no_js.cc)
  target_link_libraries(momo_engine_ui PUBLIC momo_engine_common momo_engine_graphics momo_engine_platform)
endif()

# ---- momo_engine_shell ----
add_library(momo_engine_shell STATIC
  shell/animator.cc
  shell/engine.cc
  shell/pointer_capture.cc
  shell/rasterizer.cc
  shell/render_backend.cc
  shell/shell.cc
  shell/thread_host.cc
  shell/vsync_waiter.cc)
target_link_libraries(momo_engine_shell PUBLIC momo_base momo_engine_common momo_engine_extension
                      momo_engine_graphics momo_engine_platform momo_engine_ui)

# ---- 聚合 target ----
add_library(momo_engine INTERFACE)
target_link_libraries(momo_engine INTERFACE momo_engine_shell momo_base)
add_library(momo::engine ALIAS momo_engine)

if(MOMO_BUILD_TESTS)
  add_executable(momo_engine_unittests
    common/common_unittests.cc
    common/platform_view_registry_unittests.cc
    graphics/composition_unittests.cc
    graphics/graphics_unittests.cc
    graphics/testing/golden.cc
    shell/animator_unittests.cc
    shell/pointer_capture_unittests.cc
    shell/rasterizer_unittests.cc
    shell/shell_unittests.cc
    shell/thread_host_unittests.cc)
  if(MOMO_HAVE_V8)
    target_sources(momo_engine_unittests PRIVATE
      runtime/runtime_unittests.cc ui/ui_unittests.cc extension/extension_unittests.cc)
    # 端到端加载真实的示例扩展动态库(extensions/examples, 只依赖 C ABI)。
    add_dependencies(momo_engine_unittests ext_hello ext_texture)
    target_compile_definitions(momo_engine_unittests PRIVATE
      MOMO_EXT_HELLO_LIB="$<TARGET_FILE:ext_hello>"
      MOMO_EXT_TEXTURE_LIB="$<TARGET_FILE:ext_texture>")
    if(APPLE)  # ext_webview 只在 macOS 构建(目标在 extensions/ 中定义, 此处尚不可见)
      add_dependencies(momo_engine_unittests ext_webview)
      target_compile_definitions(momo_engine_unittests PRIVATE
        MOMO_EXT_WEBVIEW_LIB="$<TARGET_FILE:ext_webview>")
    endif()
  endif()
  if(APPLE)
    target_sources(momo_engine_unittests PRIVATE graphics/gpu/metal_rasterizer_unittests.mm)
    target_link_libraries(momo_engine_unittests PRIVATE "-framework Metal")
  endif()
  # graphics 单测会包含 graphics 内部头(skia/), 需要 Skia 头文件。
  target_link_libraries(momo_engine_unittests PRIVATE momo_engine momo::skia GTest::gtest_main)
  target_compile_definitions(momo_engine_unittests PRIVATE
    MOMO_HAS_JS_RUNTIME=$<BOOL:${MOMO_HAVE_V8}>  # 仅测试用: 选择是否编译端到端 JS 用例
    MOMO_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/graphics/testdata"
    MOMO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  gtest_discover_tests(momo_engine_unittests)
endif()
