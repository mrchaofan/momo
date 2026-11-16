# extensions/ — 原生扩展 SDK、内置扩展与示例

设计文档:`docs/07-extension-system.md`。原生扩展是 Node.js 原生插件(`.node`, Node-API);需要引擎能力(平台
线程、外部纹理、平台视图)的扩展另外使用宿主能力表 `momo_host.h`。内置扩展随运行时提供(应用 `import "momo:webview"`);
三方扩展是应用自己的 npm 依赖, `examples/` 是它的样子。

| 目录 | 内容 |
|---|---|
| `sdk/include/momo/momo_host.h` | 宿主能力表(稳定 C ABI, 只在末尾追加) |
| `sdk/lib/cmake/MomoExtSdk/` | `find_package(MomoExtSdk)`:`momo::ext_sdk` + `momo_add_extension()`(产物 `<包>/build/<name>.node`;设了 `MOMO_EXTENSION_OUTPUT_DIR` 则输出到那里) |
| `webview/` | 内置扩展 `momo:webview`。ObjC++(macOS):WKWebView 平台视图。`js/index.ts`(`WebView` / `WebViewController`, 依赖 `momo:framework`)+ `native/`;随运行时构建到 `<构建目录>/embedder/builtin/webview/{index.js, ext_webview.node}` |
| `examples/hello-extension/` | 纯 C, 只用 Node-API:同步函数、字符串/字节、线程池上的 Promise。JS 门面 `js/index.js` + `js/index.d.ts` |
| `examples/texture-extension/` | C++:外部纹理, 自己的线程推帧 |
| `testing/lifecycle-addon/` | 只供测试:验证关闭顺序(docs/07 §4) |
| `sdk_selftest.cmake` | CTest `ExtensionSdk.StandaloneCxxBuild`:只凭 `<build>/sdk` 独立构建 texture 示例 |

完整的 SDK(`out/<build>/sdk/`, 或 `cmake --install <build> --prefix <dir> --component sdk`)在 `sdk/` 之外还带
Node 的头文件 `include/node/`, Windows 上另有 `libnode.dll` 的导入库 `Release/node.lib`——布局只在
`CMakeLists.txt` 的 install 规则里描述。

每个示例都是独立的插件, 不链接引擎的任何符号。它们是普通的 npm 包(`package.json` + `js/index.js` + `js/index.d.ts` +
`native/`), JS 按相对路径 `../build/<name>.node` 加载自己的插件。用它们的应用让包保持原样留在 `node_modules` 里:在自己的
构建配置里把它排除, 打包时用 `momo.include` 带上——这是应用自己的事, Momo 没有专门的配置项(docs/07 §2.2)。端到端测试(真实引擎加载真实的 `.node`)见
`examples/examples_unittests.cc`, 宿主能力表的单元测试见 `engine/native_host/native_host_unittests.cc`。
