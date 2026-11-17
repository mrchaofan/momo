# ext_webview: 内置扩展 momo:webview 的原生插件(Node 插件, 用到宿主能力表 momo_host.h)。由 extensions/CMakeLists.txt
# 加入构建, 产物输出到运行时自带模块的目录 <builtin>/webview/ext_webview.node(MOMO_EXTENSION_OUTPUT_DIR)。
#   webview_extension.cc + json.cc   平台无关: Node-API、命令协议、协议帧、cookie 筛选
#   webview_mac.mm                   macOS: WKWebView
#   webview_win.cc                   Windows: WebView2(SDK 来自 third_party/prebuilt/webview2, tools/fetch_deps.py)
set(_sources src/webview_extension.cc src/json.cc)
if(APPLE)
  momo_add_extension(ext_webview ${_sources} src/webview_mac.mm)
  target_compile_options(ext_webview PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
  target_link_libraries(ext_webview PRIVATE "-framework Cocoa" "-framework WebKit")
elseif(WIN32)
  # WebView2 SDK: 头文件 + 静态加载器(不需要随应用分发 WebView2Loader.dll)。运行时(Evergreen)由系统提供。
  set(_webview2 "${MOMO_PREBUILT_DIR}/webview2/build/native")
  if(NOT EXISTS "${_webview2}/include/WebView2.h")
    message(WARNING "WebView2 SDK not found in ${_webview2}; run: python3 tools/fetch_deps.py webview2. "
                    "The runtime is built WITHOUT momo:webview's native addon.")
    return()
  endif()
  momo_add_extension(ext_webview ${_sources} src/webview_win.cc)
  target_include_directories(ext_webview PRIVATE "${_webview2}/include")
  target_link_libraries(ext_webview PRIVATE "${_webview2}/x64/WebView2LoaderStatic.lib"
                        user32 ole32 oleaut32 shlwapi version advapi32 dcomp)
  target_compile_definitions(ext_webview PRIVATE _WIN32_WINNT=0x0A00 UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
