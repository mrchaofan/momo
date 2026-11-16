# embedder/: 宿主层(docs/01、docs/03), 对标 Flutter 的 shell/platform/<os>。
#   common/   平台无关的装配与生命周期(Application、WindowController…)+ embedder 自己的平台抽象
#             (PlatformIntegration、PlatformWindow;engine 不可见), 可用 mock 单测
#   macos/    Cocoa 实现(窗口、Metal/软件 surface、CVDisplayLink、IME、原生循环)+ 入口
#   windows/  Win32 实现 + 入口
#   headless/ 无窗口宿主(测试运行器)
# 依赖方向: embedder → engine(含 engine/platform 契约)→ base;engine 不依赖 embedder。

# 指针派发(PointerDispatcher): 平台无关的决策, 平台实现直接使用;只依赖引擎的输入类型。
add_library(momo_embedder_pointer STATIC common/pointer_dispatcher.cc)
target_link_libraries(momo_embedder_pointer PUBLIC momo_engine_platform)

# Windows 平台实现中不依赖 <windows.h> 的纯逻辑(键码映射、鼠标消息翻译、多 DPI 屏幕坐标、窗口框架命中):
# 在所有平台编译, 单测在 macOS 上同样运行;Windows 的平台实现链接它。
add_library(momo_embedder_windows_portable STATIC
  windows/win_frame.cc
  windows/win_key_mapping.cc
  windows/win_mouse_events.cc
  windows/win_screen_layout.cc)
target_link_libraries(momo_embedder_windows_portable PUBLIC momo_engine_platform momo_embedder_pointer)

# surface 的平台无关部分: GPU → 软件回退(FallbackSurface)、呈现队列与拖拽缩放同步(PresentQueue)。
# 两个平台的窗口与 surface 实现都链接它;单测在所有平台运行。
add_library(momo_embedder_surface STATIC
  common/fallback_surface.cc
  common/present_queue.cc)
target_link_libraries(momo_embedder_surface PUBLIC momo_engine_platform)

add_library(momo_embedder_common STATIC
  common/app_channel.cc
  common/app_config.cc
  common/application.cc
  common/host_channel.cc
  common/menu_channel.cc
  common/menu_model.cc
  common/platform_channels.cc
  common/tray_channel.cc
  common/window_channel.cc
  common/window_controller.cc
  common/window_manager.cc)
# 只依赖平台抽象接口: 平台实现由入口注入, common 可用 mock 单测。
target_link_libraries(momo_embedder_common PUBLIC momo::engine momo_engine_platform momo_embedder_pointer)

# 运行时自带的默认应用: 一个应用目录(package.json + 入口), 打包为 <resources>/app/。空 = 不带。
set(MOMO_DEFAULT_APP "${PROJECT_SOURCE_DIR}/examples/hello_frame" CACHE PATH
    "App directory packaged into the runtime as <resources>/app (empty = none)")

# ---- 运行时自带的 JS 模块(engine/common/settings.h 的 builtin_dir): 框架与内置扩展 ----
# 应用以 import "momo:framework" / "momo:webview" 使用它们, 它们不进应用的代码。构建到 ${MOMO_BUILTIN_DIR}/<name>/
# (与 momo_headless、Momo.exe 同目录, 即它们的 builtin/;macOS 另拷进 Momo.app 的 Resources/builtin/)。
# JS 由 framework/scripts/build_builtin.mjs 编译(需要 node 与 framework/ 下装好的 esbuild);内置扩展的原生插件
# 由 extensions/ 直接输出到同一目录(目前只有 macOS 的 webview)。
set(MOMO_BUILTIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/builtin" CACHE INTERNAL "Runtime built-in JS modules")
set(_momo_builtin_modules framework)
if(APPLE)
  list(APPEND _momo_builtin_modules webview)
endif()
find_program(MOMO_NODE node)
set(_fw "${PROJECT_SOURCE_DIR}/framework")
if(MOMO_HAVE_V8 AND MOMO_NODE AND EXISTS "${_fw}/node_modules/esbuild")
  file(GLOB_RECURSE _momo_builtin_sources CONFIGURE_DEPENDS
       "${_fw}/packages/momo/src/*.ts" "${PROJECT_SOURCE_DIR}/extensions/webview/js/*.ts")
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/builtin.stamp"
    COMMAND ${MOMO_NODE} scripts/build_builtin.mjs "${MOMO_BUILTIN_DIR}" ${_momo_builtin_modules}
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/builtin.stamp"
    DEPENDS ${_momo_builtin_sources} "${_fw}/scripts/build_builtin.mjs" "${_fw}/scripts/common.mjs"
            "${_fw}/packages/momo/package.json"
    WORKING_DIRECTORY "${_fw}"
    COMMENT "Building the runtime's built-in JS modules (${_momo_builtin_modules})"
    VERBATIM)
  add_custom_target(momo_builtin ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/builtin.stamp")
else()
  if(MOMO_HAVE_V8)
    message(WARNING "node or framework/node_modules/esbuild not found (run `npm install` in framework/): "
                    "the runtime is built WITHOUT momo:framework and the built-in extensions")
  endif()
  add_custom_target(momo_builtin ALL COMMAND ${CMAKE_COMMAND} -E make_directory "${MOMO_BUILTIN_DIR}")
endif()

if(APPLE)
  # ---- macOS 平台实现(原 platform/macos)----
  add_library(momo_embedder_macos STATIC
    macos/mac_key_mapping.mm
    macos/mac_menu.mm
    macos/mac_mouse_events.mm
    macos/mac_platform_view_input.mm
    macos/mac_native_loop.mm
    macos/mac_platform_integration.mm
    macos/mac_platform_views.mm
    macos/mac_surface_metal.mm
    macos/mac_surface_software.mm
    macos/mac_text_input.mm
    macos/mac_tray.mm
    macos/mac_vsync_displaylink.mm
    macos/mac_window.mm)
  # 实现只需要接口头(embedder/common/platform_*.h 为 header-only), 不链接引擎库。
  target_link_libraries(momo_embedder_macos
    PUBLIC momo_engine_platform momo_embedder_pointer momo_embedder_surface
    PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework CoreVideo"
            "-framework IOSurface" "-framework Metal")

  if(MOMO_BUILD_TESTS)
    add_executable(momo_embedder_macos_unittests macos/mac_platform_unittests.mm
                   macos/mac_menu_unittests.mm
                   macos/mac_mouse_events_unittests.mm
                   macos/mac_platform_view_input_unittests.mm
                   macos/mac_surface_metal_unittests.mm)
    target_link_libraries(momo_embedder_macos_unittests PRIVATE momo_embedder_macos
                          GTest::gtest_main "-framework Cocoa")
    gtest_discover_tests(momo_embedder_macos_unittests)
  endif()

  add_executable(momo_app_mac MACOSX_BUNDLE
    macos/main_mac.mm
    macos/mac_app_delegate.mm)
  target_link_libraries(momo_app_mac PRIVATE momo_embedder_common momo_embedder_macos "-framework Cocoa")
  set_target_properties(momo_app_mac PROPERTIES
    OUTPUT_NAME Momo
    MACOSX_BUNDLE_BUNDLE_NAME Momo
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.momo.app
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${MOMO_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})

  # ---- .app 打包(docs/09 §4): Resources/app/(默认应用)、Frameworks/libnode.*.dylib ----
  set(_momo_app_contents "$<TARGET_BUNDLE_CONTENT_DIR:momo_app_mac>")
  if(MOMO_DEFAULT_APP)
    add_custom_command(TARGET momo_app_mac POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${_momo_app_contents}/Resources/app"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOMO_DEFAULT_APP}"
              "${_momo_app_contents}/Resources/app"
      COMMENT "Packaging the default app into Momo.app")
  endif()
  # 自带的 JS 模块: 每次构建同步进 .app(内容随 momo_builtin 与内置扩展的插件变化)。
  add_custom_target(momo_app_builtin ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_momo_app_contents}/Resources/builtin"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOMO_BUILTIN_DIR}" "${_momo_app_contents}/Resources/builtin"
    COMMENT "Packaging the built-in JS modules into Momo.app")
  add_dependencies(momo_app_builtin momo_app_mac momo_builtin)
  if(MOMO_HAVE_V8)
    file(GLOB _momo_v8_dylibs "${MOMO_V8_RUNTIME_DIR}/*.dylib")
    # 先清空: 增量构建中换过运行时(如 V8 dylib → libnode)时, 旧文件不应留在 .app 里。
    add_custom_command(TARGET momo_app_mac POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${_momo_app_contents}/Frameworks"
      COMMAND ${CMAKE_COMMAND} -E rm -f "${_momo_app_contents}/Resources/icudtl.dat"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_momo_app_contents}/Frameworks"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_momo_v8_dylibs}
              "${_momo_app_contents}/Frameworks"
      COMMENT "Packaging libnode (V8) into Momo.app")
    # 运行期从 .app 内加载 libnode(构建树内运行时 CMake 另加绝对路径 rpath 作为兜底)。
    set_property(TARGET momo_app_mac APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
    set_property(TARGET momo_app_mac APPEND PROPERTY INSTALL_RPATH "@executable_path/../Frameworks")
  endif()
  # 发布产物(docs/09 §6): 整个 Momo.app(含上面放进去的 Resources/{app, builtin} 与 Frameworks), rpath 只留包内路径。
  install(TARGETS momo_app_mac BUNDLE DESTINATION . COMPONENT runtime)
elseif(WIN32)
  # 注意: embedder/windows 未在 Windows 上编译运行验证(实现于 macOS: 纯逻辑部分有单测, 依赖 Win32 的部分只经
  # mingw-w64 头文件交叉编译检查)。Win10 API(GetDpiForWindow、AdjustWindowRectExForDpi、
  # SetProcessDpiAwarenessContext 等)。UNICODE/NOMINMAX/WIN32_LEAN_AND_MEAN 在 MSVC 下已由 momo_compile_options
  # 定义, 这里对其他工具链兜底。
  set(_momo_win_defines _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

  # ---- Windows 平台实现 ----
  add_library(momo_embedder_windows STATIC
    windows/win_app.cc
    windows/win_caption_buttons.cc
    windows/win_displays.cc
    windows/win_menu.cc
    windows/win_native_loop.cc
    windows/win_platform_integration.cc
    windows/win_services.cc
    windows/win_surface_d3d.cc
    windows/win_surface_software.cc
    windows/win_text_input.cc
    windows/win_util.cc
    windows/win_vsync_dwm.cc
    windows/win_window.cc)
  target_link_libraries(momo_embedder_windows
    PUBLIC momo_engine_platform momo_embedder_pointer momo_embedder_windows_portable momo_embedder_surface
    PRIVATE user32 gdi32 dwmapi imm32 shcore shell32 ole32 advapi32 winmm d3d12 dxgi)
  target_compile_definitions(momo_embedder_windows PRIVATE ${_momo_win_defines})

  if(MOMO_BUILD_TESTS)
    add_executable(momo_embedder_windows_unittests windows/win_platform_unittests.cc)
    target_link_libraries(momo_embedder_windows_unittests PRIVATE momo_embedder_windows
                          GTest::gtest_main user32)
    target_compile_definitions(momo_embedder_windows_unittests PRIVATE ${_momo_win_defines})
    gtest_discover_tests(momo_embedder_windows_unittests)
  endif()

  set(_momo_app_win_sources windows/main_win.cc windows/win_app_delegate.cc)
  # 清单: Win10/11 兼容声明(分层子窗口 WS_EX_LAYERED 需要 Win8+ 声明)、PerMonitorV2 DPI 感知、UTF-8 活动代码页。
  # MSVC/clang-cl 链接器直接嵌入 .manifest;其他工具链(MinGW)经资源脚本嵌入。
  if(MSVC)
    list(APPEND _momo_app_win_sources windows/momo_app_win.manifest)
  else()
    enable_language(RC)
    list(APPEND _momo_app_win_sources windows/momo_app_win.rc)
  endif()
  add_executable(momo_app_win WIN32 ${_momo_app_win_sources})
  target_link_libraries(momo_app_win PRIVATE momo_embedder_common momo_embedder_windows shell32 user32)
  target_compile_definitions(momo_app_win PRIVATE ${_momo_win_defines})
  set_target_properties(momo_app_win PROPERTIES OUTPUT_NAME Momo)

  # ---- 打包(docs/09 §4): exe 同级 {app/(默认应用), builtin/(自带的 JS 模块), libnode DLL} ----
  if(MOMO_DEFAULT_APP)
    add_custom_command(TARGET momo_app_win POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:momo_app_win>/app"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOMO_DEFAULT_APP}"
              "$<TARGET_FILE_DIR:momo_app_win>/app"
      COMMENT "Copying the default app next to Momo.exe")
  endif()
  # libnode.dll 拷到 exe 同级(DLL 搜索路径首选 exe 目录;tools/build_libnode.py 在 Windows 上构建)。
  momo_target_use_v8_runtime(momo_app_win)
  # 发布产物(docs/09 §6): 与构建目录同一布局 {Momo.exe, libnode.dll, icudtl.dat, app/, builtin/}。
  install(TARGETS momo_app_win RUNTIME DESTINATION . COMPONENT runtime)
  install(DIRECTORY "${MOMO_BUILTIN_DIR}" DESTINATION . COMPONENT runtime)
  if(MOMO_DEFAULT_APP)
    install(DIRECTORY "${MOMO_DEFAULT_APP}/" DESTINATION app COMPONENT runtime)
  endif()
  momo_install_v8_runtime(runtime)
endif()

# ---- momo_headless: 无窗口运行一个应用(headless 测试运行器的引擎侧, docs/09 §5)----
add_executable(momo_headless headless/headless_main.cc)
target_link_libraries(momo_headless PRIVATE momo::engine)
momo_target_use_v8_runtime(momo_headless)
# 发布产物(docs/09 §6): 可执行文件与 libnode 同目录。
if(APPLE)
  set_property(TARGET momo_headless PROPERTY INSTALL_RPATH "@executable_path")
endif()
install(TARGETS momo_headless RUNTIME DESTINATION . COMPONENT headless)
install(DIRECTORY "${MOMO_BUILTIN_DIR}" DESTINATION . COMPONENT headless)
add_dependencies(momo_headless momo_builtin)
# 多配置生成器(Visual Studio)的可执行文件在 <配置>/ 子目录: 把自带的 JS 模块同步到它们旁边。
get_property(_momo_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_momo_multi_config)
  add_custom_target(momo_builtin_beside_executables ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOMO_BUILTIN_DIR}" "$<TARGET_FILE_DIR:momo_headless>/builtin"
    COMMENT "Copying the built-in JS modules next to the executables")
  add_dependencies(momo_builtin_beside_executables momo_builtin momo_headless)
endif()
momo_install_v8_runtime(headless)

if(MOMO_BUILD_TESTS)
  if(MOMO_HAVE_V8)
    add_test(NAME HeadlessSmoke
             COMMAND momo_headless ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/smoke.js
                     --width=320 --height=200 --timeout=20
                     --screenshot=${CMAKE_CURRENT_BINARY_DIR}/headless_smoke.png)
    # 框架(TS)测试: 先打包测试 bundle, 再分别在 Node(mock 宿主)与真实引擎(headless)中运行。
    if(MOMO_NODE AND EXISTS "${_fw}/node_modules/esbuild")
      add_test(NAME FrameworkBuildTests COMMAND ${MOMO_NODE} scripts/build_tests.mjs WORKING_DIRECTORY ${_fw})
      set_tests_properties(FrameworkBuildTests PROPERTIES FIXTURES_SETUP framework_bundle)
      add_test(NAME FrameworkTests.NodeMock COMMAND ${MOMO_NODE} out/test/node_main.mjs WORKING_DIRECTORY ${_fw})
      add_test(NAME FrameworkTests.EngineHeadless
               COMMAND momo_headless ${_fw}/test/engine/headless_main.js --manual-frames --timeout=120)
      set_tests_properties(FrameworkTests.NodeMock FrameworkTests.EngineHeadless
                           PROPERTIES FIXTURES_REQUIRED framework_bundle)
    endif()
  endif()
  add_executable(momo_embedder_unittests
    common/app_config_unittests.cc
    common/application_unittests.cc
    common/menu_channel_unittests.cc
    common/pointer_dispatcher_unittests.cc
    common/surface_unittests.cc
    common/window_channel_unittests.cc)
  target_link_libraries(momo_embedder_unittests PRIVATE momo_embedder_common momo_embedder_surface
                        GTest::gtest_main)
  # 仅测试用: bundle 的加载失败只在有 JS 运行时的构建里发生。
  target_compile_definitions(momo_embedder_unittests PRIVATE MOMO_HAS_JS_RUNTIME=$<BOOL:${MOMO_HAVE_V8}>)
  gtest_discover_tests(momo_embedder_unittests)

  add_executable(momo_embedder_windows_portable_unittests windows/win_portable_unittests.cc)
  target_link_libraries(momo_embedder_windows_portable_unittests PRIVATE momo_embedder_windows_portable
                        GTest::gtest_main)
  gtest_discover_tests(momo_embedder_windows_portable_unittests)
endif()
