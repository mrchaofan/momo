# 预编译三方依赖: Skia、libnode(含 V8)(docs/09 §2)。
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
  if(WIN32)
    # skia-pack 的 Windows 包带 Direct3D 12(Ganesh D3D, engine/graphics/gpu/d3d_rasterizer.cc)与 GL。
    target_compile_definitions(momo_skia INTERFACE SK_DIRECT3D SK_GL)
    target_link_libraries(momo_skia INTERFACE d3d12 dxgi d3dcompiler opengl32 dwrite user32 gdi32 ole32
                          usp10 advapi32)
  endif()
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
# V8: 来自 libnode(Node.js 26 LTS, tools/build_libnode.py 从源码构建)。momo 只使用 Node 内置的 V8,
# 不另带一份 V8: 嵌入 Node(docs/04 §1)要求 isolate 由同一份 V8 创建。
# V8 配置跟随 Node 默认: pointer compression 关、sandbox 关, 嵌入方不定义 V8_COMPRESS_POINTERS 等;
# ICU 完整数据编入 libnode(无 icudtl.dat)。头文件: include/node/{node.h, uv.h, v8*.h, libplatform/}。

set(_node_root "${MOMO_PREBUILT_DIR}/node")
file(GLOB _node_dylib "${_node_root}/lib/libnode.*.dylib")
if(MOMO_ENABLE_ASAN OR MOMO_ENABLE_TSAN)
  # libnode 未以 sanitizer 构建: sanitizer 构建不提供 momo::v8;依赖 V8 的目标与测试需在普通构建中验证。
  set(MOMO_HAVE_V8 OFF)
  message(STATUS "libnode disabled under ASan/TSan (not built with sanitizers)")
elseif(APPLE AND EXISTS "${_node_root}/.momo_dep.json" AND _node_dylib)
  add_library(momo_v8 INTERFACE)
  target_include_directories(momo_v8 SYSTEM INTERFACE "${_node_root}/include/node")
  # 必须与 V8 构建配置一致, 否则 V8::Initialize 报 "Embedder-vs-V8 build configuration mismatch"。
  # Node 默认配置下唯一影响嵌入方的是 Promise 的内部字段数(features.gypi)。
  target_compile_definitions(momo_v8 INTERFACE V8_PROMISE_INTERNAL_FIELD_COUNT=1)
  target_link_libraries(momo_v8 INTERFACE "${_node_dylib}")
  add_library(momo::v8 ALIAS momo_v8)
  # 运行期需要: dylib 目录(rpath / 打包进 .app/Contents/Frameworks)。
  set(MOMO_V8_RUNTIME_DIR "${_node_root}/lib" CACHE INTERNAL "")
  set(MOMO_HAVE_V8 ON)
  message(STATUS "V8 (libnode) prebuilt: ${_node_dylib}")
elseif(WIN32 AND EXISTS "${_node_root}/.momo_dep.json" AND EXISTS "${_node_root}/lib/libnode.lib")
  # Windows: tools/build_libnode.py 在 Windows 上产出 libnode.dll + 导入库 libnode.lib(未在 Windows 上验证)。
  add_library(momo_v8 INTERFACE)
  target_include_directories(momo_v8 SYSTEM INTERFACE "${_node_root}/include/node")
  target_compile_definitions(momo_v8 INTERFACE V8_PROMISE_INTERNAL_FIELD_COUNT=1)
  target_link_libraries(momo_v8 INTERFACE "${_node_root}/lib/libnode.lib")
  add_library(momo::v8 ALIAS momo_v8)
  # 运行期需要: libnode.dll 与 exe 同目录(DLL 搜索路径首选 exe 目录, embedder/CMakeLists.txt 拷贝)。
  set(MOMO_V8_RUNTIME_DIR "${_node_root}/lib" CACHE INTERNAL "")
  set(MOMO_HAVE_V8 ON)
  message(STATUS "V8 (libnode) prebuilt: ${_node_root}/lib/libnode.dll")
else()
  set(MOMO_HAVE_V8 OFF)
  _momo_prebuilt_missing(node)
endif()

# 给使用 V8 的可执行目标设置运行期查找路径(构建树内直接运行)。
# macOS: rpath;Windows 没有 rpath, 把 libnode.dll 拷到可执行文件旁边。
function(momo_target_use_v8_runtime target)
  if(NOT MOMO_HAVE_V8)
    return()
  endif()
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MOMO_V8_RUNTIME_DIR}/libnode.dll"
              "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Copying libnode.dll next to ${target}")
  else()
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${MOMO_V8_RUNTIME_DIR}")
  endif()
endfunction()
