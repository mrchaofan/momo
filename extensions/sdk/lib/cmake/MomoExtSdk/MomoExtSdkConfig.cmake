# MomoExtSdkConfig.cmake — Momo 原生扩展 SDK 的 CMake 包配置。
#
# SDK 目录布局(源码树 extensions/sdk/、构建产物 <build>/sdk/、安装前缀 <prefix>/ 三者相同):
#   include/momo/momo_extension.h     稳定 C ABI(唯一契约)
#   include/momo/momo_extension.hpp   C++17 header-only 便利封装(建立在 C ABI 之上)
#   lib/cmake/MomoExtSdk/             本文件
#   types/ext.d.ts                    扩展 JS 门面可用的 ext:* 模块类型模板
#
# 扩展作者用法:
#   find_package(MomoExtSdk REQUIRED)            # -DMomoExtSdk_DIR=<sdk>/lib/cmake/MomoExtSdk
#   momo_add_extension(ext_acme src/ext_acme.cc)
#
# 提供:
#   momo::ext_sdk                    INTERFACE target(头文件路径)
#   momo_add_extension(<name> <src>...)
#       建一个 SHARED 库并链接 momo::ext_sdk;隐藏可见性(只导出 momo_ext_init, 满足 momo ext verify)、
#       C11 / C++17。扩展内部可以使用异常与 RTTI, 但异常不得穿过 C ABI 边界(回调里必须捕获)。

get_filename_component(_momo_ext_sdk_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET momo::ext_sdk)
  add_library(momo::ext_sdk INTERFACE IMPORTED)
  set_target_properties(momo::ext_sdk PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_momo_ext_sdk_prefix}/include")
endif()

set(MomoExtSdk_INCLUDE_DIR "${_momo_ext_sdk_prefix}/include")
set(MomoExtSdk_TYPES_DIR "${_momo_ext_sdk_prefix}/types")
set(MomoExtSdk_API_VERSION 1)

function(momo_add_extension name)
  add_library(${name} SHARED ${ARGN})
  set_target_properties(${name} PROPERTIES
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
