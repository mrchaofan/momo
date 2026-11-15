# Momo 顶层构建骨架。详细说明见 docs/09-build-and-deps.md。
# 要求: CMake >= 3.24, Ninja, C++20。禁用异常与 RTTI(见 cmake/momo_options.cmake)。
cmake_minimum_required(VERSION 3.24)

# 最低部署 macOS 12(docs/09 §1), 须在 project() 之前设置。
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.5" CACHE STRING "Minimum macOS version (Node 26 / libnode 要求)")

# 版本号的唯一来源: @momo/framework 的 package.json(发布 tag v<version> 必须与之一致, docs/09 §6)。
# 预发布后缀(如 0.5.0-beta.1)保留在 MOMO_VERSION;CMake 的 project(VERSION) 只接受数字部分。
set(_momo_version_file "${CMAKE_CURRENT_SOURCE_DIR}/framework/packages/momo/package.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_momo_version_file}")
file(READ "${_momo_version_file}" _momo_package_json)
string(JSON MOMO_VERSION GET "${_momo_package_json}" version)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" _momo_numeric_version "${MOMO_VERSION}")

project(momo VERSION ${_momo_numeric_version} LANGUAGES C CXX)
if(MSVC)
  # The Windows Skia prebuilt uses the static release CRT in every configuration.
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
  add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)
endif()
if(APPLE)
  enable_language(OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(MOMO_BUILD_EXAMPLES "Build example extensions" ON)
option(MOMO_BUILD_TESTS    "Build unit tests"   ON)

include(momo_options)      # 全局编译选项: -fno-exceptions -fno-rtti、警告级别、sanitizer 开关
include(momo_dependencies) # FetchContent 依赖(googletest 等)
include(momo_prebuilt)     # 预编译依赖: momo::skia、momo::v8(tools/fetch_deps.py 拉取)


if(MOMO_BUILD_TESTS)
  enable_testing()
endif()

# 三层 + 基础库(单向,禁止反向链接):
#   framework(TS, 应用层) ⇢ engine(C++, 含 engine/platform 平台契约) ← embedder(C++, 宿主与平台实现)
#   所有 C++ 层 → base
add_subdirectory(base)
add_subdirectory(engine)
add_subdirectory(embedder)
add_subdirectory(extensions)
add_subdirectory(third_party)

# examples/ 与 framework/ 是 JS/TS 工程,不经 CMake 构建: 应用的 bundle 与打包都由
# tools/momo-cli 完成(momo bundle / momo build, 见 docs/09-build-and-deps.md)。
