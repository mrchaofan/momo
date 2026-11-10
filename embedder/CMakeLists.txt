# embedder/: 宿主层(docs/01、docs/03), 对标 Flutter 的 shell/platform/<os>。
#   common/   平台无关的装配与生命周期(Application、WindowController…)+ embedder 自己的平台抽象
#             (PlatformIntegration、PlatformWindow;engine 不可见), 可用 mock 单测
#   macos/    Cocoa 实现(窗口、Metal/软件 surface、CVDisplayLink、IME、原生循环)+ 入口
#   windows/  Win32 实现 + 入口
#   headless/ 无窗口宿主(测试运行器)
# 依赖方向: embedder → engine(含 engine/platform 契约)→ base;engine 不依赖 embedder。

add_library(momo_embedder_common STATIC
  common/app_config.cc
  common/application.cc
  common/asset_resolver.cc
  common/window_controller.cc)
# 只依赖平台抽象接口: 平台实现由入口注入, common 可用 mock 单测。
target_link_libraries(momo_embedder_common PUBLIC momo::engine momo_engine_platform)

if(APPLE)
  # ---- macOS 平台实现(原 platform/macos)----
  add_library(momo_embedder_macos STATIC
    macos/mac_key_mapping.mm
    macos/mac_mouse_dispatcher.mm
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
    PUBLIC momo_engine_platform
    PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework CoreVideo"
            "-framework IOSurface" "-framework Metal")

  if(MOMO_BUILD_TESTS)
    add_executable(momo_embedder_macos_unittests macos/mac_platform_unittests.mm
                   macos/mac_mouse_dispatcher_unittests.mm)
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

  # ---- .app 打包(docs/09 §4): Resources/{bundle.js, icudtl.dat}、Frameworks/libv8*.dylib ----
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
    add_custom_command(TARGET momo_app_mac POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_momo_app_contents}/Frameworks"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_momo_v8_dylibs}
              "${_momo_app_contents}/Frameworks"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOMO_V8_ICU_DATA}"
              "${_momo_app_contents}/Resources/icudtl.dat"
      COMMENT "Packaging V8 runtime into Momo.app")
    # 运行期从 .app 内加载 V8(构建树内运行时 CMake 另加绝对路径 rpath 作为兜底)。
    set_property(TARGET momo_app_mac APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
    set_property(TARGET momo_app_mac APPEND PROPERTY INSTALL_RPATH "@executable_path/../Frameworks")
  endif()
elseif(WIN32)
  # 注意: embedder/windows 未在 Windows 上编译验证(实现于 macOS, 无 Windows SDK)。
  # GPU(ANGLE, kAngleGl)surface 未实现: 窗口只用 GDI 软件 surface(win_surface_software.cc)。
  # Win10 API(GetDpiForWindow、AdjustWindowRectExForDpi、SetProcessDpiAwarenessContext 等)。
  # UNICODE/NOMINMAX/WIN32_LEAN_AND_MEAN 在 MSVC 下已由 momo_compile_options 定义, 这里对其他工具链兜底。
  set(_momo_win_defines _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

  # ---- Windows 平台实现(原 platform/windows)----
  add_library(momo_embedder_windows STATIC
    windows/win_key_mapping.cc
    windows/win_native_loop.cc
    windows/win_platform_integration.cc
    windows/win_services.cc
    windows/win_surface_software.cc
    windows/win_text_input.cc
    windows/win_util.cc
    windows/win_vsync_dwm.cc
    windows/win_window.cc)
  target_link_libraries(momo_embedder_windows
    PUBLIC momo_engine_platform
    PRIVATE user32 gdi32 dwmapi imm32 shcore shell32 ole32 advapi32)
  target_compile_definitions(momo_embedder_windows PRIVATE ${_momo_win_defines})

  if(MOMO_BUILD_TESTS)
    add_executable(momo_embedder_windows_unittests windows/win_platform_unittests.cc)
    target_link_libraries(momo_embedder_windows_unittests PRIVATE momo_embedder_windows
                          GTest::gtest_main user32)
    target_compile_definitions(momo_embedder_windows_unittests PRIVATE ${_momo_win_defines})
    gtest_discover_tests(momo_embedder_windows_unittests)
  endif()

  set(_momo_app_win_sources windows/main_win.cc windows/win_app_delegate.cc)
  if(MSVC)
    # 清单: Win10/11 兼容声明、PerMonitorV2 DPI 感知、UTF-8 活动代码页(MSVC/clang-cl 链接器自动嵌入)。
    list(APPEND _momo_app_win_sources windows/momo_app_win.manifest)
  endif()
  add_executable(momo_app_win WIN32 ${_momo_app_win_sources})
  target_link_libraries(momo_app_win PRIVATE momo_embedder_common momo_embedder_windows shell32 user32)
  target_compile_definitions(momo_app_win PRIVATE ${_momo_win_defines})
  set_target_properties(momo_app_win PROPERTIES OUTPUT_NAME Momo)

  # ---- 打包(docs/09 §4): exe 同级 {bundle.js, assets/, extensions/, icudtl.dat, V8 DLL} ----
  set(MOMO_APP_DEFAULT_BUNDLE "${PROJECT_SOURCE_DIR}/examples/hello_frame/main.js" CACHE FILEPATH
      "JS bundle copied next to Momo.exe as bundle.js (empty = none)")
  if(MOMO_APP_DEFAULT_BUNDLE)
    add_custom_command(TARGET momo_app_win POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOMO_APP_DEFAULT_BUNDLE}"
              "$<TARGET_FILE_DIR:momo_app_win>/bundle.js"
      COMMENT "Copying default JS bundle next to Momo.exe")
  endif()
  # V8 运行时: Windows 预编译包尚无来源(docs/09 §2.1), cmake/momo_prebuilt.cmake 目前只在 APPLE 上导出
  # momo::v8, 因此 Windows 上 MOMO_HAVE_V8 恒为 OFF, 下面的拷贝不会执行。接入 Windows V8 包时约定:
  #   - MOMO_V8_RUNTIME_DIR 下的 *.dll(component 构建: v8.dll、v8_libbase.dll、v8_libplatform.dll、
  #     third_party_icu / zlib 等;monolith 静态库则无需拷贝)拷到 exe 同级(DLL 搜索路径首选 exe 目录);
  #   - MOMO_V8_ICU_DATA(icudtl.dat)拷到 exe 同级(resources_dir = exe 目录, 与 mac 的 Resources 对应)。
  if(MOMO_HAVE_V8)
    file(GLOB _momo_v8_dlls "${MOMO_V8_RUNTIME_DIR}/*.dll")
    if(_momo_v8_dlls)
      add_custom_command(TARGET momo_app_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_momo_v8_dlls} "$<TARGET_FILE_DIR:momo_app_win>"
        COMMENT "Copying V8 DLLs next to Momo.exe")
    endif()
    add_custom_command(TARGET momo_app_win POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOMO_V8_ICU_DATA}"
              "$<TARGET_FILE_DIR:momo_app_win>/icudtl.dat"
      COMMENT "Copying icudtl.dat next to Momo.exe")
  endif()
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
    common/asset_resolver_unittests.cc)
  target_link_libraries(momo_embedder_unittests PRIVATE momo_embedder_common GTest::gtest_main)
  gtest_discover_tests(momo_embedder_unittests)
endif()
