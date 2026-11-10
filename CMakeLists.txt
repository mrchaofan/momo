# Momo 顶层构建骨架。详细说明见 docs/09-build-and-deps.md。
# 要求: CMake >= 3.24, Ninja, C++20。禁用异常与 RTTI(见 cmake/momo_options.cmake)。
cmake_minimum_required(VERSION 3.24)

# 最低部署 macOS 12(docs/09 §1), 须在 project() 之前设置。
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

project(momo LANGUAGES C CXX)
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

# examples/ 与 framework/ 是 JS/TS 工程,不经 CMake 构建: 示例 bundle 用 framework/scripts/build_example.mjs,
# 应用打包用 tools/momo-cli(见 docs/09-build-and-deps.md)。
