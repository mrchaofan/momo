# 扩展 SDK(docs/07): 宿主能力表的头文件 + Node 头文件(Windows 另有 libnode 的导入库)+ CMake 包配置。
# 原生扩展是 Node 插件(.node);extensions/sdk/ 是 SDK 里由本仓库编写的部分, Node 的头文件来自
# third_party/prebuilt/node(tools/build_libnode.py)。三种用法共用一份包配置:
#   - 仓库内: 直接 include 配置文件, 得到 momo::ext_sdk 与 momo_add_extension();
#   - 构建产物: 每次构建把 SDK 装到 <build>/sdk/(target momo_ext_sdk_package, 默认构建);
#   - 安装: cmake --install <build> --prefix <dir> --component sdk。
# 扩展作者: find_package(MomoExtSdk REQUIRED) + momo_add_extension(acme src/acme.cc)。
set(MOMO_EXT_SDK_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sdk)

# 引擎侧(宿主能力表的实现)只依赖 momo_host.h。
add_library(momo_ext_sdk INTERFACE)
target_include_directories(momo_ext_sdk INTERFACE ${MOMO_EXT_SDK_SOURCE_DIR}/include)

# 没有 libnode(sanitizer 构建、未拉取依赖)就没有可编译的插件, 也没有完整的 SDK。
if(NOT MOMO_HAVE_V8)
  return()
endif()

set(_momo_node_root ${MOMO_PREBUILT_DIR}/node)
set(MomoExtSdk_NODE_INCLUDE_DIR ${_momo_node_root}/include/node)
set(MomoExtSdk_NODE_LIBRARY ${_momo_node_root}/lib/libnode.lib)
include(${MOMO_EXT_SDK_SOURCE_DIR}/lib/cmake/MomoExtSdk/MomoExtSdkConfig.cmake)

# ---- 安装(组件 sdk): SDK 的布局只在这里描述 ----
install(DIRECTORY ${MOMO_EXT_SDK_SOURCE_DIR}/ DESTINATION . COMPONENT sdk PATTERN ".*" EXCLUDE)
# Node 的头文件: 不带 openssl(63 MB;需要它的插件自带)。
install(DIRECTORY ${MomoExtSdk_NODE_INCLUDE_DIR}/ DESTINATION include/node COMPONENT sdk
        PATTERN "openssl" EXCLUDE)
if(WIN32)
  install(FILES ${MomoExtSdk_NODE_LIBRARY} DESTINATION Release RENAME node.lib COMPONENT sdk)
endif()

# ---- 构建产物: <build>/sdk = 把上面的组件装进构建目录 ----
set(MOMO_EXT_SDK_BUILD_DIR ${PROJECT_BINARY_DIR}/sdk)
file(GLOB_RECURSE _momo_sdk_files CONFIGURE_DEPENDS ${MOMO_EXT_SDK_SOURCE_DIR}/*)
list(FILTER _momo_sdk_files EXCLUDE REGEX "/\\.")  # 忽略隐藏文件(.DS_Store、工具临时目录)
add_custom_command(
  OUTPUT ${MOMO_EXT_SDK_BUILD_DIR}/.stamp
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${MOMO_EXT_SDK_BUILD_DIR}
  COMMAND ${CMAKE_COMMAND} --install ${PROJECT_BINARY_DIR} --config $<CONFIG> --component sdk
          --prefix ${MOMO_EXT_SDK_BUILD_DIR}
  COMMAND ${CMAKE_COMMAND} -E touch ${MOMO_EXT_SDK_BUILD_DIR}/.stamp
  DEPENDS ${_momo_sdk_files} ${MomoExtSdk_NODE_INCLUDE_DIR}/node_version.h
  COMMENT "Staging the extension SDK into ${MOMO_EXT_SDK_BUILD_DIR}"
  VERBATIM)
add_custom_target(momo_ext_sdk_package ALL DEPENDS ${MOMO_EXT_SDK_BUILD_DIR}/.stamp)

# 示例扩展: 各自是独立的 Node 插件, 不链接引擎的任何符号。产物在各自包的 build/ 下(JS 门面从那里 import)。
#   hello   纯 C, 只用 Node-API: 同步函数、字符串/字节、后台线程上的 Promise
#   texture C++: 外部纹理(宿主表), 自己的线程出帧
#   webview ObjC++(macOS): 系统 WebView 作为平台视图嵌入(docs/05 §9)
if(MOMO_BUILD_EXAMPLES OR MOMO_BUILD_TESTS)
  add_subdirectory(examples/hello-extension/native)
  add_subdirectory(examples/texture-extension/native)
  add_subdirectory(examples/webview-extension/native)
endif()

# 端到端测试: 真实引擎(Shell + JS 运行时)加载真实的插件。依赖方向 examples → engine, 引擎自己的测试不依赖示例。
if(MOMO_BUILD_TESTS)
  add_subdirectory(testing/lifecycle-addon/native)
  add_executable(momo_extension_examples_unittests examples/examples_unittests.cc)
  add_dependencies(momo_extension_examples_unittests ext_hello ext_texture ext_lifecycle)
  target_link_libraries(momo_extension_examples_unittests PRIVATE momo::engine GTest::gtest_main)
  target_compile_definitions(momo_extension_examples_unittests PRIVATE
    MOMO_EXT_HELLO_ADDON="$<TARGET_FILE:ext_hello>"
    MOMO_EXT_TEXTURE_ADDON="$<TARGET_FILE:ext_texture>"
    MOMO_EXT_LIFECYCLE_ADDON="$<TARGET_FILE:ext_lifecycle>")
  momo_target_use_v8_runtime(momo_extension_examples_unittests)
  gtest_discover_tests(momo_extension_examples_unittests)

  # SDK 产物自检: 仅凭 <build>/sdk 独立配置并编译一个 C++ 扩展(验证头文件与包配置可用)。
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
