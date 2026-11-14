# 扩展 SDK: 稳定 C ABI 头 + C++ 便利封装 + CMake 包配置(docs/07)。
# extensions/sdk/ 本身就是 SDK 的发布布局(include/、lib/cmake/MomoExtSdk/、types/), 三种用法共用一份配置:
#   - 仓库内: 直接 include 配置文件, 得到 momo::ext_sdk 与 momo_add_extension();
#   - 构建产物: 每次构建把 SDK 拷到 <build>/sdk/(target momo_ext_sdk_package, 默认构建);
#   - 安装: cmake --install <build> --prefix <dir> --component sdk。
# 扩展作者: find_package(MomoExtSdk REQUIRED) + momo_add_extension(ext_x src/x.cc)。
set(MOMO_EXT_SDK_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
include(${MOMO_EXT_SDK_SOURCE_DIR}/lib/cmake/MomoExtSdk/MomoExtSdkConfig.cmake)

# 引擎侧(extension 宿主实现)也只依赖这些头文件。momo::ext_sdk 是 IMPORTED target, 只在本目录及子目录
# 可见, 所以给其他目录用的 momo_ext_sdk 直接指向头文件目录。
add_library(momo_ext_sdk INTERFACE)
target_include_directories(momo_ext_sdk INTERFACE ${MOMO_EXT_SDK_SOURCE_DIR}/include)

# ---- 构建产物: <build>/sdk ----
set(MOMO_EXT_SDK_BUILD_DIR ${PROJECT_BINARY_DIR}/sdk)
file(GLOB_RECURSE _momo_sdk_files CONFIGURE_DEPENDS ${MOMO_EXT_SDK_SOURCE_DIR}/*)
list(FILTER _momo_sdk_files EXCLUDE REGEX "/\\.")  # 忽略隐藏文件(.DS_Store、工具临时目录)
add_custom_command(
  OUTPUT ${MOMO_EXT_SDK_BUILD_DIR}/.stamp
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${MOMO_EXT_SDK_BUILD_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MOMO_EXT_SDK_BUILD_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${MOMO_EXT_SDK_SOURCE_DIR}/include ${MOMO_EXT_SDK_BUILD_DIR}/include
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${MOMO_EXT_SDK_SOURCE_DIR}/lib ${MOMO_EXT_SDK_BUILD_DIR}/lib
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${MOMO_EXT_SDK_SOURCE_DIR}/types ${MOMO_EXT_SDK_BUILD_DIR}/types
  COMMAND ${CMAKE_COMMAND} -E touch ${MOMO_EXT_SDK_BUILD_DIR}/.stamp
  DEPENDS ${_momo_sdk_files}
  COMMENT "Staging the extension SDK into ${MOMO_EXT_SDK_BUILD_DIR}")
add_custom_target(momo_ext_sdk_package ALL DEPENDS ${MOMO_EXT_SDK_BUILD_DIR}/.stamp)

# ---- 安装 ----
install(DIRECTORY ${MOMO_EXT_SDK_SOURCE_DIR}/include/ DESTINATION include COMPONENT sdk
        PATTERN ".*" EXCLUDE)
install(DIRECTORY ${MOMO_EXT_SDK_SOURCE_DIR}/lib/cmake/MomoExtSdk/ DESTINATION lib/cmake/MomoExtSdk
        COMPONENT sdk PATTERN ".*" EXCLUDE)
install(DIRECTORY ${MOMO_EXT_SDK_SOURCE_DIR}/types/ DESTINATION types COMPONENT sdk
        PATTERN ".*" EXCLUDE)

# 示例扩展(docs/11 M5): 各自是独立的动态库, 不链接引擎的任何符号, 只用 C ABI。
#   hello   纯 C: 函数绑定、Promise、消息通道(同时是 ABI 兼容性验证: 不依赖 C++ 运行时)
#   texture C++(momo_extension.hpp): 外部纹理注入 + 帧回调
#   webview ObjC++(macOS): 系统 WebView 作为平台视图嵌入(docs/05 §9)
if(MOMO_BUILD_EXAMPLES OR MOMO_BUILD_TESTS)
  add_subdirectory(examples/hello-extension/native)
  add_subdirectory(examples/texture-extension/native)
  add_subdirectory(examples/webview-extension/native)
endif()

# 示例扩展的端到端测试: 经 ExtensionHost 加载真实的扩展动态库(依赖方向 examples → engine,
# 引擎自己的测试不依赖示例)。
if(MOMO_BUILD_TESTS AND MOMO_HAVE_V8)
  add_executable(momo_extension_examples_unittests examples/examples_unittests.cc)
  add_dependencies(momo_extension_examples_unittests ext_hello ext_texture)
  target_link_libraries(momo_extension_examples_unittests PRIVATE momo::engine GTest::gtest_main)
  target_compile_definitions(momo_extension_examples_unittests PRIVATE
    MOMO_EXT_HELLO_LIB="$<TARGET_FILE:ext_hello>"
    MOMO_EXT_TEXTURE_LIB="$<TARGET_FILE:ext_texture>")
  if(TARGET ext_webview)
    add_dependencies(momo_extension_examples_unittests ext_webview)
    target_compile_definitions(momo_extension_examples_unittests PRIVATE
      MOMO_EXT_WEBVIEW_LIB="$<TARGET_FILE:ext_webview>")
  endif()
  momo_target_use_v8_runtime(momo_extension_examples_unittests)
  gtest_discover_tests(momo_extension_examples_unittests)
endif()

# ABI 校验(momo ext verify): 只导出 momo_ext_init、无 C++ 符号、不链接引擎内部库。
find_program(MOMO_NODE node)
if(MOMO_BUILD_TESTS AND MOMO_NODE)
  set(_momo_verify_options)
  if(MSVC)
    get_filename_component(_momo_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
    find_program(MOMO_DUMPBIN dumpbin HINTS "${_momo_msvc_bin}")
    if(MOMO_DUMPBIN)
      list(APPEND _momo_verify_options "--dumpbin=${MOMO_DUMPBIN}")
    endif()
  endif()
  foreach(ext ext_hello ext_texture ext_webview)
    if(TARGET ${ext})
      add_test(NAME ExtensionAbiVerify.${ext}
               COMMAND ${MOMO_NODE} ${PROJECT_SOURCE_DIR}/tools/momo-cli/momo.mjs ext verify
                       $<TARGET_FILE:${ext}> ${_momo_verify_options})
    endif()
  endforeach()
endif()

# SDK 产物自检: 仅凭 <build>/sdk 独立配置并编译一个 C++ 扩展(验证头文件与包配置可用)。
if(MOMO_BUILD_TESTS)
  add_test(NAME ExtensionSdk.StandaloneCxxBuild
           COMMAND ${CMAKE_COMMAND}
                   -DSDK_DIR=${MOMO_EXT_SDK_BUILD_DIR}
                   -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/examples/texture-extension/native
                   -DWORK_DIR=${PROJECT_BINARY_DIR}/sdk_selftest
                   -DGENERATOR=${CMAKE_GENERATOR}
                   -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                   -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/sdk_selftest.cmake)
endif()
