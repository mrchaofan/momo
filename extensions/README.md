# extensions/ — 原生扩展 SDK 与示例

设计文档:`docs/07-extension-system.md`。原生扩展是 Node.js 原生插件(`.node`, Node-API);需要引擎能力(平台
线程、外部纹理、平台视图)的扩展另外使用宿主能力表 `momo_host.h`。

| 目录 | 内容 |
|---|---|
| `sdk/include/momo/momo_host.h` | 宿主能力表(稳定 C ABI, 只在末尾追加) |
| `sdk/lib/cmake/MomoExtSdk/` | `find_package(MomoExtSdk)`:`momo::ext_sdk` + `momo_add_extension()`(产物 `<包>/build/<name>.node`) |
| `examples/hello-extension/` | 纯 C, 只用 Node-API:同步函数、字符串/字节、线程池上的 Promise |
| `examples/texture-extension/` | C++:外部纹理, 自己的线程推帧 |
| `examples/webview-extension/` | ObjC++(macOS):WKWebView 平台视图 + JS 门面 `WebView` / `WebViewController` |
| `testing/lifecycle-addon/` | 只供测试:验证关闭顺序(docs/07 §4) |
| `sdk_selftest.cmake` | CTest `ExtensionSdk.StandaloneCxxBuild`:只凭 `<build>/sdk` 独立构建 texture 示例 |

完整的 SDK(`out/<build>/sdk/`, 或 `cmake --install <build> --prefix <dir> --component sdk`)在 `sdk/` 之外还带
Node 的头文件 `include/node/`, Windows 上另有 `libnode.dll` 的导入库 `Release/node.lib`——布局只在
`CMakeLists.txt` 的 install 规则里描述。

每个示例都是独立的插件, 不链接引擎的任何符号。端到端测试(真实引擎加载真实的 `.node`)见
`examples/examples_unittests.cc`, 宿主能力表的单元测试见 `engine/extension/extension_unittests.cc`。
