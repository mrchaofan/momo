# 预编译三方依赖: Skia、V8(docs/09 §2)。
# 包由 tools/fetch_deps.py 按 cmake/deps.lock.json 下载到 third_party/prebuilt/<name>/。
# 找到时导出 IMPORTED/INTERFACE target `momo::skia`、`momo::v8`;缺失时不报错, 只是不提供
# (现有不依赖它们的 target 照常构建)。MOMO_REQUIRE_PREBUILT=ON 时缺失即报错。
#
# 规则(docs/10 §1): Skia 头只出现在 graphics/text/assets, v8.h 只出现在 runtime/ui 实现文件。

option(MOMO_REQUIRE_PREBUILT "Fail configuration if Skia/V8 prebuilts are missing" OFF)

set(MOMO_PREBUILT_DIR "${PROJECT_SOURCE_DIR}/third_party/prebuilt" CACHE PATH
    "Directory with prebuilt third-party packages (tools/fetch_deps.py)")

function(_momo_prebuilt_missing name)
  set(msg "Prebuilt ${name} not found in ${MOMO_PREBUILT_DIR}/${name}; run: python3 tools/fetch_deps.py ${name}")
  if(MOMO_REQUIRE_PREBUILT)
    message(FATAL_ERROR "${msg}")
  else()
    message(STATUS "${msg}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Skia (JetBrains skia-pack: 静态库, 含 skparagraph/skshaper/skunicode + 改名 ICU(_skiko 后缀))

set(_skia_root "${MOMO_PREBUILT_DIR}/skia")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(_skia_out "${_skia_root}/out/Release-macos-arm64")
elseif(APPLE)
  set(_skia_out "${_skia_root}/out/Release-macos-x64")
elseif(WIN32)
  set(_skia_out "${_skia_root}/out/Release-windows-x64")
endif()

if(EXISTS "${_skia_root}/.momo_dep.json" AND EXISTS "${_skia_out}")
  add_library(momo_skia INTERFACE)
  # SYSTEM: 三方头不受 -Wall -Werror 约束。头文件以 skia 根为基准("include/core/SkCanvas.h")。
  target_include_directories(momo_skia SYSTEM INTERFACE "${_skia_root}")
  # 与预编译产物一致的配置宏(SK_RELEASE 必须: 否则 debug 构建下 SK_DEBUG 改变内联代码/布局)。
  target_compile_definitions(momo_skia INTERFACE
    SK_RELEASE SK_GANESH SK_SHAPER_HARFBUZZ_AVAILABLE SK_SHAPER_UNICODE_AVAILABLE
    SK_UNICODE_AVAILABLE SK_UNICODE_ICU_IMPLEMENTATION)
  set(_skia_libs skparagraph skshaper skunicode_icu skunicode_core skresources skia
                 harfbuzz icu png jpeg jpeg12 jpeg16 webp webp_sse41 wuffs zlib expat skcms
                 dng_sdk piex bentleyottmann)
  foreach(lib IN LISTS _skia_libs)
    set(_path "${_skia_out}/${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    if(EXISTS "${_path}")
      target_link_libraries(momo_skia INTERFACE "${_path}")
    endif()
  endforeach()
  if(APPLE)
    target_compile_definitions(momo_skia INTERFACE SK_METAL SK_GL)
    target_link_libraries(momo_skia INTERFACE
      "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText"
      "-framework CoreServices" "-framework Foundation" "-framework Metal"
      "-framework QuartzCore" "-framework OpenGL" "-framework AppKit")
  endif()
  add_library(momo::skia ALIAS momo_skia)
  set(MOMO_HAVE_SKIA ON)
  message(STATUS "Skia prebuilt: ${_skia_out}")
else()
  set(MOMO_HAVE_SKIA OFF)
  _momo_prebuilt_missing(skia)
endif()

# ---------------------------------------------------------------------------
# V8 (macOS: Homebrew bottle, component 构建 —— 多个 dylib + icudtl.dat;
#     use_custom_libcxx=false 与系统 libc++ ABI 一致;pointer compression 开、sandbox 关)

set(_v8_root "${MOMO_PREBUILT_DIR}/v8")
set(_v8_libexec "${_v8_root}/libexec")
if(MOMO_ENABLE_ASAN OR MOMO_ENABLE_TSAN)
  # 该预编译包含 partition_alloc 的 malloc shim, 与 ASan/TSan 的 malloc 拦截冲突(进程启动即崩溃)。
  # sanitizer 构建不提供 momo::v8;依赖 V8 的目标与测试需在普通构建中验证。
  set(MOMO_HAVE_V8 OFF)
  message(STATUS "V8 prebuilt disabled under ASan/TSan (partition_alloc malloc shim conflicts)")
elseif(APPLE AND EXISTS "${_v8_root}/.momo_dep.json" AND EXISTS "${_v8_libexec}/libv8.dylib")
  add_library(momo_v8 INTERFACE)
  target_include_directories(momo_v8 SYSTEM INTERFACE "${_v8_libexec}/include")
  # 必须与 V8 构建配置一致, 否则 V8::Initialize 报 "Embedder-vs-V8 build configuration mismatch"。
  target_compile_definitions(momo_v8 INTERFACE V8_COMPRESS_POINTERS V8_31BIT_SMIS_ON_64BIT_ARCH)
  target_link_libraries(momo_v8 INTERFACE
    "${_v8_libexec}/libv8.dylib"
    "${_v8_libexec}/libv8_libplatform.dylib"
    "${_v8_libexec}/libv8_libbase.dylib")
  add_library(momo::v8 ALIAS momo_v8)
  # 运行期需要: dylib 目录(rpath / 打包进 .app/Contents/Frameworks)与 ICU 数据文件。
  set(MOMO_V8_RUNTIME_DIR "${_v8_libexec}" CACHE INTERNAL "")
  set(MOMO_V8_ICU_DATA "${_v8_libexec}/icudtl.dat" CACHE INTERNAL "")
  set(MOMO_HAVE_V8 ON)
  message(STATUS "V8 prebuilt: ${_v8_libexec}")
else()
  set(MOMO_HAVE_V8 OFF)
  _momo_prebuilt_missing(v8)
endif()

# 给使用 V8 的可执行目标设置运行期查找路径(构建树内直接运行)。
function(momo_target_use_v8_runtime target)
  if(MOMO_HAVE_V8)
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${MOMO_V8_RUNTIME_DIR}")
    target_compile_definitions(${target} PRIVATE MOMO_V8_ICU_DATA="${MOMO_V8_ICU_DATA}")
  endif()
endfunction()
