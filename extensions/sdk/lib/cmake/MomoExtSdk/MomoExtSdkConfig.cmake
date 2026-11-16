# MomoExtSdkConfig.cmake — Momo 原生扩展 SDK 的 CMake 包配置(docs/07)。
#
# 原生扩展是 Node.js 原生插件(.node)。SDK 就是编译一个能被 Momo 加载的插件所需的全部:
#   include/momo/momo_host.h   宿主能力表(平台线程、外部纹理、平台视图)
#   include/node/              Momo 运行时所带 Node 的头文件(node_api.h 等;同 Electron 发布自己的头文件)
#   Release/node.lib           仅 Windows: libnode.dll 的导入库(路径同 node-gyp 的 --nodedir 约定)
#   lib/cmake/MomoExtSdk/      本文件
#
# 扩展作者用法(CMakeLists.txt 放在 <包>/native/):
#   find_package(MomoExtSdk REQUIRED)            # -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk
#   momo_add_extension(acme src/acme.cc)         # → <包>/build/acme.node
# 用 node-gyp 的扩展: node-gyp rebuild --nodedir=<sdk>。
#
# 提供:
#   momo::ext_sdk                    INTERFACE target: 头文件路径与链接方式
#   momo_add_extension(<name> <src>...)
#       建一个 Node 插件 <name>.node, 输出到 CMakeLists.txt 上一级目录的 build/(node-gyp 的惯例位置,
#       包里的 JS 按相对路径 import);设了 MOMO_EXTENSION_OUTPUT_DIR 则输出到那里(不想写进源码树的构建)。
#       隐藏可见性、C11 / C++17。插件内部可以使用异常与 RTTI, 但异常不得
#       穿过 Node-API 与宿主表的回调边界。

get_filename_component(_momo_ext_sdk_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# Momo 源码树内 Node 的头文件与导入库不在 SDK 目录下, 由 extensions/CMakeLists.txt 预先给出。
if(NOT MomoExtSdk_NODE_INCLUDE_DIR)
  set(MomoExtSdk_NODE_INCLUDE_DIR "${_momo_ext_sdk_prefix}/include/node")
endif()
if(WIN32 AND NOT MomoExtSdk_NODE_LIBRARY)
  set(MomoExtSdk_NODE_LIBRARY "${_momo_ext_sdk_prefix}/Release/node.lib")
endif()
set(MomoExtSdk_INCLUDE_DIR "${_momo_ext_sdk_prefix}/include")
set(MomoExtSdk_API_VERSION 1)

if(NOT TARGET momo::ext_sdk)
  add_library(momo::ext_sdk INTERFACE IMPORTED)
  set_target_properties(momo::ext_sdk PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MomoExtSdk_INCLUDE_DIR};${MomoExtSdk_NODE_INCLUDE_DIR}"
    INTERFACE_COMPILE_DEFINITIONS "BUILDING_NODE_EXTENSION")
  if(WIN32)
    # Node-API 的符号在 libnode.dll 里: 直接链接它的导入库, 插件的导入表指向 libnode.dll(与应用 exe 的名字无关)。
    set_target_properties(momo::ext_sdk PROPERTIES INTERFACE_LINK_LIBRARIES "${MomoExtSdk_NODE_LIBRARY}")
  elseif(APPLE)
    # Node-API 的符号在加载时从进程(libnode)解析。
    set_target_properties(momo::ext_sdk PROPERTIES INTERFACE_LINK_OPTIONS "LINKER:-undefined,dynamic_lookup")
  endif()
endif()

function(momo_add_extension name)
  add_library(${name} MODULE ${ARGN})
  if(MOMO_EXTENSION_OUTPUT_DIR)
    get_filename_component(_output_dir "${MOMO_EXTENSION_OUTPUT_DIR}" ABSOLUTE)
  else()
    get_filename_component(_output_dir "${CMAKE_CURRENT_SOURCE_DIR}/../build" ABSOLUTE)
  endif()
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    # 生成器表达式: 多配置生成器(Visual Studio)不再追加 Debug/Release 子目录。
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_output_dir}>"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  target_link_libraries(${name} PRIVATE momo::ext_sdk)
endfunction()
