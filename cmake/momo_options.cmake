# 全局编译选项(docs/09 §1, docs/10 §1)。
# 选项挂在 INTERFACE target `momo_compile_options` 上, 只作用于 momo 自身的 target,
# 不污染 FetchContent 引入的三方库。

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MOMO_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(MOMO_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(MOMO_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

add_library(momo_compile_options INTERFACE)

if(MSVC)
  # /wd4100: 未使用的形参(与 GCC/Clang 的 -Wno-unused-parameter 对齐)。
  target_compile_options(momo_compile_options INTERFACE /W4 /wd4100 /EHs-c- /GR- /utf-8 /permissive- /Zc:__cplusplus)
  target_compile_definitions(momo_compile_options INTERFACE _CRT_SECURE_NO_WARNINGS _HAS_EXCEPTIONS=0 NOMINMAX
                             WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  if(MOMO_WARNINGS_AS_ERRORS)
    target_compile_options(momo_compile_options INTERFACE /WX)
  endif()
else()
  target_compile_options(momo_compile_options INTERFACE
    -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti)
  if(MOMO_WARNINGS_AS_ERRORS)
    target_compile_options(momo_compile_options INTERFACE -Werror)
  endif()
  # Objective-C++: ARC
  target_compile_options(momo_compile_options INTERFACE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

if(MOMO_ENABLE_ASAN AND MOMO_ENABLE_TSAN)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()
if(MOMO_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
if(MOMO_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()

# 统一 include 根: 头文件一律以仓库根为基准(如 "base/status.h")。
target_include_directories(momo_compile_options INTERFACE ${PROJECT_SOURCE_DIR})
