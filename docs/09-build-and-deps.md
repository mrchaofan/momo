# 09 · 构建、依赖、打包、测试与发布

本文是这些内容的唯一来源：工具链（§1）、第三方依赖（§2）、CMake target 与产物（§3）、JS 的构建与应用打包（§4）、测试怎么跑（§5）、发布（§6）。

## 1. 工具链

- CMake ≥ 3.24 + Ninja；macOS：Xcode clang，最低部署版本 **macOS 13.5**（Node 26 / libnode 的要求，顶层 `CMakeLists.txt`）；Windows：MSVC / clang-cl（未验证）。
- C++20；全局 `-fno-exceptions -fno-rtti`（MSVC `/EHs-c- /GR-`），`-Wall -Wextra`，默认 warning-as-error（`MOMO_WARNINGS_AS_ERRORS`）。选项挂在 `momo_compile_options` 上，只作用于 momo 自己的 target（`cmake/momo_options.cmake`）。
- Python 3：`tools/fetch_deps.py`；构建 libnode 需要 Python ≥ 3.10。
- Node.js + npm：构建运行时自带的 JS 模块要用 node 和 `framework/` 下装好的 esbuild（`cd framework && npm install`）。没有时 CMake 给出警告，构建出的运行时不带 `momo:framework`（§4.1）。
- 没有 `CMakePresets.json`，也没有跑测试的日常 CI：CI 只有发布流程（§6.2）。

从零构建的四步命令见 [README](../README.md)「快速开始」。

CMake 选项：`MOMO_BUILD_TESTS`、`MOMO_BUILD_EXAMPLES`（示例扩展）、`MOMO_ENABLE_ASAN` / `MOMO_ENABLE_TSAN`、`MOMO_WARNINGS_AS_ERRORS`、`MOMO_REQUIRE_PREBUILT`（缺预编译包即报错；默认只提示）、`MOMO_DEFAULT_APP`（打进运行时的默认应用目录，默认 `examples/hello_frame`；空 = 不带）。

## 2. 第三方依赖

Skia 与 Node 从源码构建成本很高，策略是**预编译产物**，版本与 SHA256 锁定在 `cmake/deps.lock.json`。`third_party/` 不入库源码，只有接入冒烟测试（`third_party/smoke_test`）和下载结果（`prebuilt/`，已 gitignore）。

| 依赖 | 版本 | 来源 | 形态 |
|---|---|---|---|
| Skia（含 skparagraph / skshaper / skunicode） | m144（JetBrains skia-pack） | 下载预编译包（`macos-arm64`、`windows-x64`） | 静态库；ICU 符号带后缀，不与 Node 的 ICU 冲突；含 Metal 后端，Windows 包带 Direct3D 12 |
| libnode（含 V8、libuv、ICU） | Node.js 26.13.0（V8 14.6、libuv 1.52、ICU 78） | **源码构建**（`tools/build_libnode.py`；Node 官方不发布 libnode） | 单个 `libnode.*.dylib` + 头文件。Node 默认配置：pointer compression 关、sandbox 关（与 Node 原生插件 ABI 一致）；ICU 数据编入二进制 |
| WebView2 SDK | 1.0.4314.35（NuGet `Microsoft.Web.WebView2`） | 下载（仅 `windows-x64`） | 头文件 + 静态加载器 `WebView2LoaderStatic.lib`；只有 `momo:webview` 的 Windows 插件用它。运行时（Evergreen）由系统提供 |
| googletest | 1.15.2 | CMake FetchContent（`cmake/momo_dependencies.cmake`，仅 `MOMO_BUILD_TESTS`） | — |
| esbuild、typescript（npm） | 见 `framework/package.json` | `npm install` | 编译运行时自带的 JS 模块、框架测试的打包、类型检查 |

- **V8 来自 libnode，不另带一份**：嵌入 Node 要求 isolate 由 Node 自带的 V8 创建，所以 `momo::v8` 直接指向 libnode 的头文件与动态库。
- `tools/fetch_deps.py`：按块并行发 Range 请求、断点续传、SHA256 校验；解压前检查路径穿越，先解到全新空目录再整体替换。下载缓存在 `third_party/.downloads/`。`--mirror=<dir>` 用离线镜像，`--jobs`、`--force`；带 `build` 的依赖（libnode）转交 `build_libnode.py`。
- `tools/build_libnode.py`：取源码包并校验 → `configure --shared` → `ninja libnode` → 安装头文件与 dylib（install name 改为 `@rpath/...`，ad-hoc 重签名）→ 写戳记；已安装且版本/参数一致时跳过。首次构建约 20~40 分钟。Windows 上用 `vcbuild.bat` 构建 `libnode.dll` + 导入库（未验证）。
- `cmake/momo_prebuilt.cmake` 根据 `third_party/prebuilt/<name>/.momo_dep.json` 戳记导出 `momo::skia`、`momo::v8`。接入是否正确由 `momo_thirdparty_smoketest` 验证（Skia 光栅与段落排版、V8 执行脚本与 Intl）。
- **sanitizer 构建没有 V8**（libnode 未以 sanitizer 构建）：`MOMO_ENABLE_ASAN` / `MOMO_ENABLE_TSAN` 时 `momo::v8` 自动关闭，JS 相关的代码与测试不编译，引擎用占位驱动。

## 3. CMake target 与产物

库 target 命名 `momo_<layer>_<module>`；可见性由 `target_link_libraries` 的 PRIVATE/PUBLIC 控制，实现"engine 不见 embedder、平台实现只见 `engine/platform` 契约"。

| target | 产物 / 用途 |
|---|---|
| `momo_base`、`momo_engine_*`（聚合 `momo::engine`）、`momo_embedder_common`、`momo_embedder_macos` / `momo_embedder_windows` | 各层静态库 |
| `momo_app_mac` | `Momo.app`：`Contents/MacOS/Momo`、`Contents/Resources/{app, builtin}/`、`Contents/Frameworks/libnode.*.dylib`。Windows 是 `momo_app_win`（`Momo.exe`） |
| `momo_builtin` | 运行时自带的 JS 模块 `<build>/embedder/builtin/<name>/{package.json, index.js}`（§4.1）；`momo_app_builtin` 把它同步进 `Momo.app` |
| `momo_headless` | 无窗口宿主（离屏软件 surface）：冒烟测试、在真实引擎上跑框架测试 |
| `momo_ext_sdk_package` | 扩展 SDK `<build>/sdk/`（docs/07 §5） |
| `ext_hello` / `ext_texture` / `ext_webview` | 示例三方扩展（产物在各自包的 `build/*.node`）；内置扩展 webview 的插件（macOS / Windows，输出到 `builtin/webview/`） |
| `*_unittests` | `momo_base_unittests`、`momo_engine_unittests`、`momo_embedder_unittests`（mock 平台）、`momo_embedder_macos_unittests`、`momo_embedder_windows_portable_unittests`、`momo_extension_examples_unittests` |

## 4. JS 的构建与应用打包

JS 分三块，各有各的构建，互不相干：

| | 谁构建 | 产物在哪 |
|---|---|---|
| 运行时自带的模块（框架、内置扩展） | Momo 的 CMake（§4.1） | 运行时的 `<resources>/builtin/` |
| 应用自己的代码（含它用的三方包） | 应用自己选的工具（§4.2） | 应用目录（`main`、`node_modules`） |
| 可分发的应用 | `momo build`（§4.3）：只复制文件，不编译 | `<Name>.app` / `<Name>/` |

`framework/` 是 npm workspace，里面两个发布的包：`packages/momo`（`@momo/framework`，以 TS 源码发布；应用只用它做类型检查，实现由运行时提供）与 `packages/cli`（`@momo/cli`，命令 `momo`，不依赖源码树，也没有 npm 依赖）。

### 4.1 运行时自带的模块

框架与内置扩展是运行时的一部分，应用以 `import "momo:framework"`、`import "momo:webview"` 使用（解析见 ARCHITECTURE.md §4.6），不进应用的代码。

```
Momo.app/Contents/Resources/        # Windows: Momo.exe 同级;momo_headless: 可执行文件同级
├── builtin/
│   ├── framework/{package.json, index.js}                    # 框架
│   └── webview/{package.json, index.js, ext_webview.node}    # 内置扩展: JS 与原生插件在同一目录
└── app/                                                      # 应用
```

- `node framework/scripts/build_builtin.mjs <out-dir> <module>...` 用 esbuild 把每个模块的 TS 编译成单文件 ESM，并生成 `package.json`（版本取框架的版本）。`momo:*` 与 `*.node` 的导入原样保留。esbuild 的设置在 `scripts/common.mjs`，与测试打包共用。
- webview 的原生插件由 CMake 直接输出到 `builtin/webview/ext_webview.node`（`extensions/CMakeLists.txt` 设 `MOMO_EXTENSION_OUTPUT_DIR`）。

### 4.2 应用

- **应用 = 一个目录或一个 JS 文件**（`engine/common/app_layout.h` 的 `LocateApp`），开发时与打包后走同一条路：
  - 目录：`<dir>/package.json` 的 `main` 是入口（缺省 `index.js`），资源在 `<dir>/assets/`。读不到 `package.json`、JSON 不合法、`main` 不是字符串时启动即报错。
  - 文件：它自己就是入口，资源在它旁边的 `assets/`。
  - 入口文件、`assets/` 是否存在不在定位时检查：入口不存在是加载时的失败，应用可以没有资源。
  - 应用路径是 `Momo` / `momo_headless` 的第一个位置参数；`Momo` 不给时用应用包内的 `<resources>/app`。
- **源码怎么变成 `main` 是应用自己的事**，Momo 不提供也不要求任何编译工具。要求只有两条：产物是 ESM；`momo:*` 的导入原样保留。纯 JS 应用不需要编译（`examples/hello_frame`）。仓库里的 View 示例用一行 esbuild（借用 `framework/node_modules` 里的 esbuild，所以示例自己不需要 `npm install`）：

  ```bash
  esbuild src/main.ts --bundle --format=esm --platform=neutral '--external:momo:*' --sourcemap=inline --outfile=dist/main.js
  ```
- **类型检查**：tsconfig 的 `paths` 把 `momo:framework` 指到 `@momo/framework` 的 `src/index.ts`（仓库内见 `framework/tsconfig.json`，它同时覆盖示例）。`momo:webview` 目前没有单独发布的类型包，仓库内同样靠 `paths` 指到 `extensions/webview/js/index.ts`。
- **三方包**要么由应用的构建工具编译进 `main`，要么留在 `node_modules` 里由 Node 加载；带原生插件的包必须是后者（docs/07 §2）。

### 4.3 打包：`momo build`

CLI 只有这一个命令（`framework/packages/cli/momo.mjs`，文件头注释是完整的用法说明），不走 CMake：

```
momo build <app-dir> [--out=<dir>] [--runtime=<Momo.app|dir>] [--name=<App>] [--dmg]
```

复制运行时（`--runtime=` 或环境变量 `MOMO_RUNTIME` 给出：发布产物解压后的内容，或自己的构建）→ 把应用目录里选中的文件**原样、保持相对位置**放进 `<resources>/app/`（`<out>` 默认 `<app-dir>/dist`）：

- macOS：`<Name>.app/Contents/Resources/{builtin/, app/…}`；改 Info.plist 的显示名、bundle id 与图标，重新 ad-hoc 签名；`--dmg` 再用 `hdiutil` 产出 `<Name>.dmg`。
- Windows：`<Name>/{<Name>.exe, *.dll, builtin/, app/}`（未验证）。其他平台没有运行时，只输出 `<Name>/app/`。

`package.json` 里的约定：运行时只读 `main`；CLI 另读

| 字段 | 含义 |
|---|---|
| `momo.id` | 应用标识，反域名写法，**必填**：写进 macOS 的 bundle id；系统按它存放用户数据（V8 code cache、WebView 的网站数据），两个应用用同一个标识就会共用这些数据 |
| `momo.name` | 应用名（缺省取 `name`，`--name` 优先） |
| `momo.icon` | `.icns`（macOS） |
| `momo.include` / `momo.ignore` | 追加 / 去掉哪些文件 |

- **选哪些文件只按文件名决定**：`package.json`、`main`、`assets/**` 总是带上，`momo.include` 追加，`momo.ignore` 从全部结果里去掉。通配：`*` 匹配一段路径里的任意字符，`**` 匹配任意层目录，`?` 匹配一个字符，以 `/` 结尾表示整个目录。跟随符号链接。
- **CLI 不编译、不分析文件内容、不知道依赖**：打包前先用应用自己的构建脚本产出 `main`；留在 `node_modules` 里的包由应用写进 `momo.include`（如 `"node_modules/better-sqlite3/"`），它依赖的包也要写上。
- 没匹配到任何文件的 include（写错了或依赖没安装）报错；`main` 不在选中的文件里（还没编译，或被 ignore 去掉）也报错。

## 5. 测试

| 测试 | 命令 | 覆盖 | 什么时候跑 |
|---|---|---|---|
| 全部 C++ 与框架 | `cd out/debug && ctest -j8` | 下面各行的总和 | 提交前 |
| base | `ctest -R 'EventLoop\|Object\|Pipeline\|TaskUtil'` | 事件循环、延迟析构、背压 | 改 `base/` |
| engine | `ctest -R 'Shell\|Animator\|Rasterizer\|Hub\|UiBindings\|Golden\|AppLayout\|JsRuntime'` | Shell 时序、背压、绑定、golden、运行时 | 改 `engine/` |
| embedder | `ctest -R 'Application\|WindowChannel\|PointerDispatcher\|PresentQueue\|Mac'` | 启动/关闭顺序（mock 平台）、通道、指针派发、macOS 实现 | 改 `embedder/` |
| 扩展 | `ctest -R 'Extension\|NativeHost'` | 宿主能力表、端到端（真实引擎加载真实的 `.node`）、关闭顺序、SDK 独立构建 | 改扩展宿主或 SDK |
| 框架（Node mock） | `cd framework && npm test` | 全部 TS 测试 + counter / todo / 浏览器示例端到端 | 改 `framework/` |
| 框架（真实引擎） | `ctest -R FrameworkTests` | 同一组 TS 测试跑在 `momo_headless` 上（`FrameworkTests.EngineHeadless`），以及 Node mock 与三个示例（`FrameworkTests.NodeMock`、`Example*`） | 改绑定、宿主通道或框架 |
| 冒烟 | `ctest -R HeadlessSmoke` | 无窗口完整跑一个应用并截图 | 任何引擎改动 |
| 类型检查 | `cd framework && npm run typecheck` | 框架、测试与示例 | 改 TS |

- 只跑名字里包含某个子串的框架用例：`npm run build:tests`，再 `node out/test/node_main.mjs <子串>`。`build:tests` 同时产出给 `momo_headless` 用的 `out/test/momo_tests.js`（`momo:*` 为外部导入；入口 `framework/test/engine/headless_main.js`）。
- Golden 基准图更新（确认是预期内的视觉变化后）：`MOMO_UPDATE_GOLDENS=1 ctest -R Golden`。
- 不开窗口复现问题：`momo_headless <app> --width=800 --height=600 --screenshot=out.png`，用法见 `embedder/headless/headless_main.cc` 开头的注释。
- 没有 GPU、显示器或不允许绑定端口的环境里，Metal、CVDisplayLink、inspector 网络用例会显示 `Skipped`，不是失败；这些路径要在真机上验证。
- ASan / TSan：`-DMOMO_ENABLE_ASAN=ON` / `-DMOMO_ENABLE_TSAN=ON`（没有 V8，见 §2）。

为什么这样分层见 ARCHITECTURE.md §7。

## 6. 发布

### 6.1 发布产物

预编译运行时作为 GitHub Release 的附件发布，命名与布局参照 Electron（`<p>` 是 Node 的 `process.platform-process.arch`：`darwin-arm64`、`win32-x64`）：

| 产物 | 内容 |
|---|---|
| `momo-v<version>-<p>.zip` | 运行时，自带框架与内置扩展（`builtin/`）与默认应用。解压后直接传给 `momo build --runtime=` |
| `momo-headless-v<version>-<p>.zip` | `momo_headless` + libnode + `builtin/`：无窗口运行应用的测试运行器 |
| `momo-ext-sdk-v<version>-<p>.zip` | 原生扩展 SDK（docs/07 §5） |
| `momo-framework-v<version>.tgz` | `@momo/framework` 的 npm 包：框架的 TS 源码，应用用它做类型检查 |
| `momo-cli-v<version>.tgz` | `@momo/cli` 的 npm 包（版本与 `@momo/framework` 相同，打包时校验） |
| `SHASUMS256.txt` | 以上全部文件的 SHA256 |

- **版本号**只有一个来源：`framework/packages/momo/package.json` 的 `version`。CMake 读它（`MOMO_VERSION`，写进 Info.plist），打包脚本读它，发布 tag 必须是 `v<version>`。
- **包的内容**由 CMake 安装组件决定：`runtime`、`headless`（`embedder/CMakeLists.txt`）、`sdk`（`extensions/CMakeLists.txt`）。`tools/package_release.py` 对每个组件执行 `cmake --install --component` → ad-hoc 签名（macOS）→ 压缩，自己不描述布局。
- macOS 二进制是 **ad-hoc 签名**，没有 Developer ID 签名与公证；不带调试符号包。

```bash
cmake -S . -B out/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DMOMO_BUILD_TESTS=OFF -DMOMO_BUILD_EXAMPLES=OFF
cmake --build out/release
python3 tools/package_release.py --build-dir out/release --out out/dist --platform-independent
```

### 6.2 发布流程（`.github/workflows/release.yml`）

1. 把 `framework/packages/momo/package.json`、`framework/packages/cli/package.json`、`framework/package.json` 的 `version` 改为新版本并合入 `main`。
2. 推送 tag `v<version>`（带 `-` 的版本发为 prerelease）。
3. CI 在 `macos-15`（arm64）与 `windows-2022`（x64）上各自：校验 tag 与版本一致 → `fetch_deps.py` → Release 构建 → 打包 → **用解压出的 headless 包**跑 `embedder/headless/testdata/smoke.js`；另在仓库之外只凭 CLI 的 npm 包与解压出的运行时 `momo build` 一个应用。全部成功后汇总产物、生成 `SHASUMS256.txt`、创建 GitHub Release。

- 手动触发，或改动发布链路的 PR 与 `main` 提交，走同一套构建与打包，但产物只作为 workflow artifact 上传。
- libnode 的安装结果按 Node 版本 + `build_libnode.py` 内容缓存；tag 触发的运行只能读到默认分支上建立的缓存。
- 发布构建关闭 `MOMO_WARNINGS_AS_ERRORS`，不构建也不运行单元测试。
- **Windows 任务尚未在 GitHub runner 上运行验证**。
