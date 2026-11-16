# ext_webview: 系统 WebView 平台视图示例(ObjC++ 的 Node 插件, 用到宿主能力表 momo_host.h)。
# 产物: ../build/ext_webview.node。目前只实现 macOS(WKWebView);其他平台不构建。
# 独立构建(扩展作者):
#   cmake -S . -B out -DMomoExtSdk_DIR=<momo 构建目录>/sdk/lib/cmake/MomoExtSdk && cmake --build out
if(NOT COMMAND momo_add_extension)
  cmake_minimum_required(VERSION 3.24)
  project(ext_webview LANGUAGES CXX OBJCXX)
  find_package(MomoExtSdk 1 REQUIRED)
endif()
if(APPLE)
  momo_add_extension(ext_webview src/ext_webview.mm)
  target_compile_options(ext_webview PRIVATE -fobjc-arc)
  target_link_libraries(ext_webview PRIVATE "-framework Cocoa" "-framework WebKit"
    "-framework UniformTypeIdentifiers")
endif()
