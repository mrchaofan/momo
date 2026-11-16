# ext_webview: 内置扩展 momo:webview 的原生插件(ObjC++ 的 Node 插件, 用到宿主能力表 momo_host.h)。
# 目前只实现 macOS(WKWebView)。由 extensions/CMakeLists.txt 加入构建, 产物输出到运行时自带模块的目录
# <builtin>/webview/ext_webview.node(MOMO_EXTENSION_OUTPUT_DIR)。
momo_add_extension(ext_webview src/ext_webview.mm)
target_compile_options(ext_webview PRIVATE -fobjc-arc)
target_link_libraries(ext_webview PRIVATE "-framework Cocoa" "-framework WebKit")
