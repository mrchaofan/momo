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

add_library(momo_embedder_common STATIC
  common/app_channel.cc
  common/app_config.cc
  common/application.cc
  common/asset_resolver.cc
  common/host_channel.cc
  common/platform_channels.cc
  common/window_channel.cc
  common/window_controller.cc
  common/window_manager.cc)
# 只依赖平台抽象接口: 平台实现由入口注入, common 可用 mock 单测。
target_link_libraries(momo_embedder_common PUBLIC momo::engine momo_engine_platform momo_embedder_pointer)

if(APPLE)
  # ---- macOS 平台实现(原 platform/macos)----
  add_library(momo_embedder_macos STATIC
    macos/mac_key_mapping.mm
    macos/mac_mouse_events.mm
    macos/mac_platform_view_input.mm
    macos/mac_native_loop.mm
    macos/mac_platform_integration.mm
    macos/mac_platform_views.mm
    macos/mac_surface_metal.mm
    macos/mac_surface_software.mm
    macos/mac_text_input.mm
    macos/mac_vsync_displaylink.mm
    macos/mac_window.mm)
  # 实现只需要接口头(embedder/common/platform_*.h 为 header-only), 不链接引擎库。
  target_link_libraries(momo_embedder_macos
    PUBLIC momo_engine_platform momo_embedder_pointer
    PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework CoreVideo"
            "-framework IOSurface" "-framework Metal")

  if(MOMO_BUILD_TESTS)
    add_executable(momo_embedder_macos_unittests macos/mac_platform_unittests.mm
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
    MACOSX_BUNDLE_SHORT_VERSION_STRING 0.0.1)

  # ---- .app 打包(docs/09 §4): Resources/bundle.js、Frameworks/libnode.*.dylib ----
  set(MOMO_APP_DEFAULT_BUNDLE "${PROJECT_SOURCE_DIR}/examples/hello_frame/main.js" CACHE FILEPATH
      "JS bundle packaged as Momo.app/Contents/Resources/bundle.js (empty = none)")
  set(_momo_app_contents "$<TARGET_BUNDLE_CONTENT_DIR:momo_app_mac>")
  if(MOMO_APP_DEFAULT_BUNDLE)
    add_custom_command(TARGET momo_app_mac POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_momo_app_contents}/Resources"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOMO_APP_DEFAULT_BUNDLE}"
              "${_momo_app_contents}/Resources/bundle.js"
      COMMENT "Packaging default JS bundle into Momo.app")
  endif()
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
    PUBLIC momo_engine_platform momo_embedder_pointer momo_embedder_windows_portable
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

  # ---- 打包(docs/09 §4): exe 同级 {bundle.js, assets/, extensions/, libnode DLL} ----
  set(MOMO_APP_DEFAULT_BUNDLE "${PROJECT_SOURCE_DIR}/examples/hello_frame/main.js" CACHE FILEPATH
      "JS bundle copied next to Momo.exe as bundle.js (empty = none)")
  if(MOMO_APP_DEFAULT_BUNDLE)
    add_custom_command(TARGET momo_app_win POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOMO_APP_DEFAULT_BUNDLE}"
              "$<TARGET_FILE_DIR:momo_app_win>/bundle.js"
      COMMENT "Copying default JS bundle next to Momo.exe")
  endif()
  # libnode.dll 拷到 exe 同级(DLL 搜索路径首选 exe 目录;tools/build_libnode.py 在 Windows 上构建)。
  momo_target_use_v8_runtime(momo_app_win)
endif()

# ---- momo_headless: 无窗口运行 bundle(headless 测试运行器的引擎侧, docs/09 §5)----
add_executable(momo_headless headless/headless_main.cc)
target_link_libraries(momo_headless PRIVATE momo::engine)
momo_target_use_v8_runtime(momo_headless)

if(MOMO_BUILD_TESTS)
  if(MOMO_HAVE_V8)
    add_test(NAME HeadlessSmoke
             COMMAND momo_headless --bundle=${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/smoke.js
                     --width=320 --height=200 --timeout=20
                     --screenshot=${CMAKE_CURRENT_BINARY_DIR}/headless_smoke.png)
    # 框架(TS)测试: 先打包测试 bundle, 再分别在 Node(mock 宿主)与真实引擎(headless)中运行。
    find_program(MOMO_NODE node)
    set(_fw "${PROJECT_SOURCE_DIR}/framework")
    if(MOMO_NODE AND EXISTS "${_fw}/node_modules/esbuild")
      add_test(NAME FrameworkBuildTests COMMAND ${MOMO_NODE} scripts/build_tests.mjs WORKING_DIRECTORY ${_fw})
      set_tests_properties(FrameworkBuildTests PROPERTIES FIXTURES_SETUP framework_bundle)
      add_test(NAME FrameworkTests.NodeMock COMMAND ${MOMO_NODE} out/test/node_main.mjs WORKING_DIRECTORY ${_fw})
      add_test(NAME FrameworkTests.EngineHeadless
               COMMAND momo_headless --manual-frames --timeout=120
                       --bundle=${_fw}/test/engine/headless_main.js)
      set_tests_properties(FrameworkTests.NodeMock FrameworkTests.EngineHeadless
                           PROPERTIES FIXTURES_REQUIRED framework_bundle)
    endif()
  endif()
  add_executable(momo_embedder_unittests
    common/app_config_unittests.cc
    common/application_unittests.cc
    common/asset_resolver_unittests.cc
    common/pointer_dispatcher_unittests.cc
    common/window_channel_unittests.cc)
  target_link_libraries(momo_embedder_unittests PRIVATE momo_embedder_common GTest::gtest_main)
  gtest_discover_tests(momo_embedder_unittests)

  add_executable(momo_embedder_windows_portable_unittests windows/win_portable_unittests.cc)
  target_link_libraries(momo_embedder_windows_portable_unittests PRIVATE momo_embedder_windows_portable
                        GTest::gtest_main)
  gtest_discover_tests(momo_embedder_windows_portable_unittests)
endif()
