# extensions/ — 扩展 SDK 与示例

设计文档:`docs/07-extension-system.md`。`sdk/` 就是扩展 SDK 的发布布局:构建时原样放到 `out/<build>/sdk/`,
也可 `cmake --install <build> --prefix <dir> --component sdk` 安装。

| 目录 | 内容 |
|---|---|
| `sdk/include/momo/momo_extension.h` | 稳定 C ABI(唯一契约, v1 已定稿, 只增不改) |
| `sdk/include/momo/momo_extension.hpp` | C++17 header-only 便利封装(`Extension`/`Host`/`Call`/`MOMO_EXTENSION`), 建立在 C ABI 之上 |
| `sdk/lib/cmake/MomoExtSdk/` | `find_package(MomoExtSdk)`:`momo::ext_sdk` + `momo_add_extension()` |
| `sdk/types/` | `ext.d.ts`:扩展 JS 门面可用的 `ext:*` 模块类型模板 |
| `examples/hello-extension/` | 纯 C:同步函数、字符串/字节、worker 线程 + Promise、消息通道 `hello/upper` |
| `examples/texture-extension/` | C++ 封装:纹理注入, 帧回调驱动 worker 渲染等离子动画, 推帧 |
| `sdk_selftest.cmake` | CTest `ExtensionSdk.StandaloneCxxBuild`:只凭 `<build>/sdk` 独立构建 texture 示例 |

每个示例都是独立动态库, 用 `momo_add_extension()` 构建(仓库外用 `find_package(MomoExtSdk)`);`momo ext verify` 检查只导出 `momo_ext_init`、不链接引擎内部库(CTest `ExtensionAbiVerify.*`)。端到端测试见 `engine/extension/extension_unittests.cc`。
