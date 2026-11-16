# 09 · 构建系统与依赖管理

## 1. 工具链

- CMake ≥ 3.24 + Ninja;Windows: clang-cl(与 Skia/V8 预编译产物 ABI 匹配性最好)或 MSVC;macOS: Xcode clang,最低部署 macOS 13.5(Node 26 / libnode 的要求)。
- Node.js + npm:构建运行时自带的 JS 模块(框架、内置扩展)要用 node 和 `framework/` 下装好的 esbuild(`cd framework && npm install`, §4.1)。没有时 CMake 给出警告, 构建出的运行时不带 `momo:framework`。
- C++20;全局 `-fno-exceptions -fno-rtti`(MSVC `/EHs-c- /GR-`),warning-as-error。
- 尚未提供 `CMakePresets.json`,直接用 `cmake -S . -B out/debug -G Ninja`。CI 目前只有发布流程(§6),没有跑测试的日常 CI(§5)。

## 2. 第三方依赖策略

Skia 与 V8 从源码构建成本极高(GN/depot_tools、小时级编译),策略:**预编译产物优先,源码构建留作可选路径**。

| 依赖 | 用途 | 引入方式 |
|---|---|---|
| Skia(含 skparagraph/skshaper/skunicode) | 图形/文本 | 自建 CI 产出预编译包(按平台×架构×CRT),`momo_dependencies.cmake` 下载校验后以 IMPORTED target `momo::skia` 暴露 |
| V8(monolith) | JS 引擎 | 同上,`momo::v8`;版本跟随稳定分支,升级节奏每季度评估 |
| ICU | V8 与 skunicode 共用 | 随 V8/Skia 包提供,注意**两者必须同一 ICU 版本**(坑:符号冲突) |
| spdlog | 日志 | FetchContent,header-only 模式 |
| googletest | 测试 | FetchContent |
| rapidjson 或 simdjson | 配置解析 | FetchContent(禁异常配置) |
| esbuild(npm) | 编译运行时自带的 JS 模块(§4.1);框架测试的打包 | `framework/package.json` 的 devDependency, `npm install` |

所有预编译包带 SHA256 锁定在 `cmake/deps.lock.json`;`tools/fetch_deps.py` 支持离线镜像(`--mirror`,对 libnode 的源码包同样生效)。

### 2.1 当前落地(macOS arm64)

| 依赖 | 版本 | 来源 | 形态 | 备注 |
|---|---|---|---|---|
| Skia | m144-e2e6623374-4 | JetBrains skia-pack release | 静态库(skia、skparagraph、skshaper、skunicode_icu、harfbuzz、icu…) | 系统 libc++(`std::__1`);ICU 符号带 `_skiko` 后缀, 不与 V8 的 ICU 冲突;含 Metal/GL 后端 |
| libnode(含 V8) | Node.js 26.13.0 LTS(V8 14.6.202.34、libuv 1.52.1、ICU 78) | 源码构建(`tools/build_libnode.py`;Node 官方不发布 libnode) | 单个 `libnode.147.dylib`(约 137 MB)+ 头文件 `include/node/{node.h, uv.h, v8*.h, v8-inspector*.h, libplatform/}` | 系统 libc++;Node 默认配置:pointer compression 关、sandbox 关(与 Node 原生插件 ABI 一致),嵌入方只需定义 `V8_PROMISE_INTERNAL_FIELD_COUNT=1`;ICU 完整数据编入二进制(无 `icudtl.dat`) |

与原规划的差异与已知限制:

- **V8 来自 libnode,不另带一份**。嵌入 Node(docs/04 §1)要求 isolate 由 Node 自带的 V8 创建,所以 `momo::v8` 直接指向
  libnode 的头文件与 dylib。`build_libnode.py` 用 `fetch_deps.py` 的下载器取源码包并校验 SHA256(同样支持 `--mirror`/`--jobs`)→ `configure --shared --without-npm --ninja` → `ninja libnode`
  → 安装头文件(Node 的 `tools/install.py --headers-only`,另补 V8 inspector 头)与 dylib(install name 改为 `@rpath/...`,
  ad-hoc 重签名)→ 写戳记;已安装且版本/参数一致时跳过。首次构建约 20~40 分钟,需要 Python ≥ 3.10(Node 的 configure 要求,
  脚本会在 PATH 与 uv 管理的解释器中查找)。打包 `.app` 时 dylib 拷进 `Contents/Frameworks`。
- **最低 macOS 13.5**:Node 26 的构建目标,项目部署目标随之提高。
- **libnode 不用于 ASan/TSan 构建**(未以 sanitizer 构建):sanitizer 构建中 `momo::v8` 不可用(`cmake/momo_prebuilt.cmake` 自动关闭)。
- Windows:Skia 已锁定 `windows-x64` 包(带 Direct3D 12, `momo::skia` 定义 `SK_DIRECT3D`);libnode 由 `build_libnode.py` 在
  Windows 上用 `vcbuild.bat release x64 dll nonpm` 构建出 `libnode.dll` + 导入库 `libnode.lib`(需要 Visual Studio 的 Clang 编译器和 LLVM MSBuild 工具集，以及 PATH 中的 NASM；Node 26 的 Temporal 支持需要 Rust 工具链),
  `momo_target_use_v8_runtime` 把 dll 拷到可执行文件旁边。以上 Windows 路径均未在 Windows 上运行验证。

### 2.2 获取与接入

```bash
python3 tools/fetch_deps.py            # 当前平台全部依赖 → third_party/prebuilt/{skia,node}(node 从源码构建)
```

下载器按块并行发 Range 请求(很多代理对单连接限速),支持断点续传,并做 SHA256 校验;解压前检查路径穿越,先解到全新空目录再整体替换。`cmake/momo_prebuilt.cmake` 根据 `third_party/prebuilt/<name>/.momo_dep.json` 戳记导出 `momo::skia`、`momo::v8`(指向 libnode);缺包时只提示、不报错(`-DMOMO_REQUIRE_PREBUILT=ON` 时缺包即报错)。接入是否正确由 `momo_thirdparty_smoketest` 验证:Skia 光栅绘制与段落排版、V8 执行脚本与 Intl。

## 3. 目标组织

```
momo::engine          (聚合 momo_engine_{platform,common,shell,runtime,ui,graphics,text,assets,native_host})
momo_embedder_common  (静态库: 装配 + 平台抽象接口)
momo_embedder_{macos,windows}  (静态库: 平台实现, 只链接 momo_engine_platform)
momo_app_win / momo_app_mac   (可执行文件/App Bundle)
momo_builtin          (运行时自带的 JS 模块: <build>/embedder/builtin/{framework, webview}, §4.1)
momo_ext_sdk          (INTERFACE: 原生扩展的宿主能力表头文件 momo_host.h;引擎侧用)
momo_ext_sdk_package  (构建产物 <build>/sdk: 完整的扩展 SDK, docs/07 §5)
```

可见性由 `target_link_libraries` 的 PRIVATE/PUBLIC 严格控制实现"engine 不见 embedder、平台实现只见 engine/platform 契约"。

## 4. JS/TS 构建与应用打包

JS 分三块, 各有各的构建, 互不相干:

| | 谁构建 | 产物在哪 |
|---|---|---|
| 运行时自带的模块(框架、内置扩展) | Momo 的 CMake(§4.1) | 运行时的 `<resources>/builtin/` |
| 应用自己的代码(含它用的三方包) | 应用自己选的工具(§4.2) | 应用目录(`main`、`node_modules`) |
| 可分发的应用 | `momo build`(§4.3):只复制文件, 不编译 | `<Name>.app` / `<Name>/` |

framework/ 是 npm workspace, 里面两个发布的包:`packages/momo`(`@momo/framework`, 以 TS 源码发布;应用只用它做类型检查, 实现由运行时提供)与 `packages/cli`(`@momo/cli`, 命令 `momo`, 不依赖源码树, 也没有 npm 依赖)。

### 4.1 运行时自带的模块

框架与内置扩展是运行时的一部分, 应用以 `import "momo:framework"`、`import "momo:webview"` 使用(解析见 docs/04 §4), 不进应用的代码。目录(`engine/common/settings.h` 的 `builtin_dir`):

```
Momo.app/Contents/Resources/        # Windows: Momo.exe 同级;momo_headless: 可执行文件同级
├── builtin/
│   ├── framework/{package.json, index.js}                    # 框架
│   └── webview/{package.json, index.js, ext_webview.node}    # 内置扩展(仅 macOS): JS 与原生插件在同一目录
└── app/                                                      # 应用
```

构建(`embedder/CMakeLists.txt` 的 target `momo_builtin`):

- `node framework/scripts/build_builtin.mjs <out-dir> <module>...` 用 esbuild 把每个模块的 TS 编译成单文件 ESM `<out-dir>/<name>/index.js`, 并生成 `package.json`(`main: index.js`, 版本取框架的版本)。`<out-dir>` 是 `<构建目录>/embedder/builtin`, 与 `momo_headless`、`Momo.exe` 同目录;模块是 `framework`, macOS 再加 `webview`。`momo:*`(模块之间的引用、引擎的原生模块)与 `*.node` 的导入原样保留。
- webview 的原生插件由 CMake 直接输出到 `builtin/webview/ext_webview.node`(`extensions/CMakeLists.txt` 设 `MOMO_EXTENSION_OUTPUT_DIR`)。
- macOS 上 target `momo_app_builtin` 每次构建把这个目录同步进 `Momo.app/Contents/Resources/builtin/`。
- **需要 node 和 `framework/` 下装好的 esbuild**(`cd framework && npm install`)。找不到时 CMake 配置阶段给出警告, `builtin/` 是空目录, 运行时不带 `momo:framework` 与内置扩展(只有引擎的 `momo:ui`、`momo:services`)。

### 4.2 应用

- **应用 = 一个目录或一个 JS 文件**(`engine/common/app_layout.h` 的 `LocateApp`), 开发时与打包后走同一条路:
  - 目录: `<dir>/package.json` 的 `main` 是入口(缺省 `index.js`), 资源在 `<dir>/assets/`。读不到 `package.json`、JSON 不合法、`main` 不是字符串时启动即报错。
  - 文件: 它自己就是入口, 资源在它旁边的 `assets/`。
  - 入口文件、`assets/` 是否存在不在定位时检查: 入口不存在是加载时的失败, 应用可以没有资源。
  - 应用路径是 `Momo` / `momo_headless` 的第一个位置参数(`Momo --dev examples/counter`);`Momo` 不给时用应用包内的 `<resources>/app`。
- **源码怎么变成 `main` 是应用自己的事**, Momo 不提供也不要求任何编译工具。要求只有两条:产物是 ESM;`momo:*` 的导入原样保留(由运行时解析)。纯 JS 应用不需要编译(`examples/hello_frame`)。仓库里的 View 示例用一行 esbuild(`examples/counter/package.json` 的 `scripts.build`):

  ```bash
  esbuild src/main.ts --bundle --format=esm --platform=neutral '--external:momo:*' --sourcemap=inline --outfile=dist/main.js
  ```

  产物里只有业务自己的代码(counter 约 12 KB)。示例不依赖任何 npm 包, 不需要 `npm install`:`cd examples/counter && npm run build`(脚本借用 `framework/node_modules/.bin/esbuild`), 再 `Momo --dev examples/counter`。
- **类型检查**:tsconfig 的 `paths` 把 `momo:framework` 指到 `@momo/framework` 的 `src/index.ts`(仓库内见 `framework/tsconfig.json`, 它同时覆盖示例)。`momo:webview` 目前没有单独发布的类型包, 仓库内同样靠 `paths` 指到 `extensions/webview/js/index.ts`。
- **三方包**(包括带原生插件的三方扩展)同样是应用自己的事:要么由应用的构建工具编译进 `main`, 要么留在 `node_modules` 里由 Node 加载。带原生插件的包必须留在 `node_modules`:应用在自己的构建配置里把它排除(esbuild `--external:<包名>`), 打包时用 `momo.include` 带上(docs/07 §2.2)。
- CMake 只做一件相关的事:把 `MOMO_DEFAULT_APP`(一个应用目录, 默认 `examples/hello_frame`;空 = 不带)拷为运行时的 `<resources>/app/`, 作为自带的默认应用;`momo build` 用应用替换它。
- startup snapshot(`tools/make_snapshot`)未实现,见 docs/04 §1。

### 4.3 打包:`momo build`

CLI 只有这一个命令(`framework/packages/cli/momo.mjs`, 文件头注释是完整的用法说明), 不走 CMake:

```
momo build <app-dir> [--out=<dir>] [--runtime=<Momo.app|dir>] [--name=<App>] [--dmg]
```

复制运行时(`--runtime=` 或环境变量 `MOMO_RUNTIME` 给出;发布产物解压后的内容或自己的构建)→ 把应用目录里选中的文件**原样、保持相对位置**放进 `<resources>/app/`(`<out>` 默认 `<app-dir>/dist`):

- macOS: `<Name>.app/Contents/Resources/{builtin/, app/{package.json, <main>, assets/, …}}`;改 Info.plist 的显示名与图标(`momo.icon`), 重新 ad-hoc 签名;`--dmg` 再用 `hdiutil` 产出 `<Name>.dmg`
- Windows: `<Name>/{<Name>.exe, *.dll, icudtl.dat, builtin/, app/}`(exe 同级即资源目录;`--runtime=<含 Momo.exe 的目录>`, 未验证)
- 其他平台没有运行时, 只输出 `<Name>/app/`

`package.json` 里的约定:运行时只读 `main`;CLI 另读 `momo.include` / `momo.ignore`(带上/去掉哪些文件)、`momo.name`(应用名, 缺省取 `name`, `--name` 优先)、`momo.icon`(`.icns`, macOS)。

- **选哪些文件只按文件名决定**:`package.json`、`main`、`assets/**` 总是带上, `momo.include` 追加, `momo.ignore` 从全部结果里去掉。通配:`*` 匹配一段路径里的任意字符, `**` 匹配任意层目录, `?` 匹配一个字符, 以 `/` 结尾表示整个目录。跟随符号链接(链接进 `node_modules` 的本地包复制为它指向的文件)。
- **CLI 不编译、不分析文件内容、不知道依赖**:打包前先用应用自己的构建脚本产出 `main`;留在 `node_modules` 里的包由应用写进 `momo.include`(如 `"node_modules/better-sqlite3/"`), 它依赖的包也要写上。
- 没匹配到任何文件的 include(写错了或依赖没安装)报错;`main` 不在选中的文件里(不存在——还没编译, 或被 ignore 去掉)也报错。

## 5. 测试与 CI

- C++ 单元测试:gtest;engine 可在 headless(软件后端 + mock platform)下全量跑。
- 框架测试:同一组 `framework/test/*_test.ts` 既在 Node + mock `momo:ui` 下跑(`npm test`),也由 `momo_headless`(真实引擎 + 软件光栅)运行(CTest `FrameworkTests.EngineHeadless`)。
- Golden 像素测试:软件光栅化 PNG 比对(双平台跑同一组,保证一致性)。
- 跑测试的 CI 尚未建立(规划:win-x64 / mac-arm64 × debug(ASan)/ release;TSan 每日任务;发布流程见 §6,它不跑 `ctest`);ASan/TSan 构建可在本地用 `-DMOMO_ENABLE_ASAN=ON` / `-DMOMO_ENABLE_TSAN=ON` 生成(此时 V8 不可用, 见 §2.1)。

## 6. 打包与分发

`momo build`(`@momo/cli`)面向应用开发者:预编译的 engine 运行时 + 用户 JS/资源/扩展 → 产出 .app / 目录(后续 MSIX/DMG)。即应用开发者**无需本机编译 C++**,只有扩展作者需要工具链——这是 npm 式开发体验的关键。

### 6.1 发布产物

预编译运行时作为 GitHub Release 的附件发布,命名与布局参照 Electron(`<p>` 是 Node 的 `process.platform-process.arch`:`darwin-arm64`、`win32-x64`):

| 产物 | 内容 |
|---|---|
| `momo-v<version>-<p>.zip` | 运行时, 自带框架与内置扩展(`builtin/`)。macOS:`Momo.app`(`Contents/Resources/{builtin, app}`);Windows:`Momo.exe`、`libnode.dll`、`icudtl.dat`、`builtin/`、`app/`(默认应用)。另有 `version` 文件。解压后直接传给 `momo build --runtime=` |
| `momo-headless-v<version>-<p>.zip` | `momo_headless` + libnode + `builtin/`:无窗口运行应用的测试运行器 |
| `momo-ext-sdk-v<version>-<p>.zip` | 原生扩展 SDK(docs/07 §5):`include/momo/momo_host.h`、`include/node/`(Node 头文件, 不含 openssl)、`lib/cmake/MomoExtSdk/`;Windows 另有 `Release/node.lib`(`libnode.dll` 的导入库, 未运行验证) |
| `momo-framework-v<version>.tgz` | `@momo/framework` 的 npm 包(与平台无关):框架的 TS 源码, 应用用它做类型检查(实现由运行时提供) |
| `momo-cli-v<version>.tgz` | `@momo/cli` 的 npm 包(与平台无关;版本与 `@momo/framework` 相同, 打包时校验) |
| `SHASUMS256.txt` | 以上全部文件的 SHA256 |

- **版本号**只有一个来源:`framework/packages/momo/package.json` 的 `version`。CMake 读它(`MOMO_VERSION`,写进 `Momo.app` 的 Info.plist),打包脚本读它,发布 tag 必须是 `v<version>`。
- **包的内容**由 CMake 安装组件决定:`runtime`、`headless`(`embedder/CMakeLists.txt`)、`sdk`(`extensions/CMakeLists.txt`)。`tools/package_release.py` 对每个组件执行 `cmake --install --component` → ad-hoc 签名(macOS)→ 压缩,自己不描述布局。安装时 rpath 只保留包内路径(`@executable_path/../Frameworks`)。
- macOS 二进制是 **ad-hoc 签名**,没有 Developer ID 签名与公证;不带调试符号包。

```bash
(cd framework && npm install)          # esbuild: 运行时自带的模块要用它编译(§4.1)
cmake -S . -B out/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DMOMO_BUILD_TESTS=OFF -DMOMO_BUILD_EXAMPLES=OFF
cmake --build out/release
python3 tools/package_release.py --build-dir out/release --out out/dist --platform-independent
```

### 6.2 发布流程(`.github/workflows/release.yml`)

1. 把 `framework/packages/momo/package.json`、`framework/packages/cli/package.json`(以及 workspace 根的 `framework/package.json`)的 `version` 改为新版本并合入 `main`。
2. 推送 tag `v<version>`(带 `-` 的版本,如 `v0.5.0-beta.1`,发为 prerelease)。
3. CI 在 `macos-15`(arm64)与 `windows-2022`(x64)上各自:校验 tag 与版本一致 → `fetch_deps.py` → Release 构建 → 打包 → **用解压出的 headless 包**跑 `embedder/headless/testdata/smoke.js`(验证包自带全部运行期依赖);产出 npm 包的平台另在仓库之外只凭 CLI 的 npm 包与解压出的运行时 `momo build` 一个应用(验证 CLI 不依赖源码树, 运行时带有 `builtin/framework`)。全部成功后汇总产物、生成 `SHASUMS256.txt`、创建 GitHub Release。

- 手动触发,或改动发布链路(workflow、`package_release.py`、`build_libnode.py`、`fetch_deps.py`、`cmake/`)的 PR 与 `main` 提交,走同一套构建与打包,但产物只作为 workflow artifact 上传,不创建 Release。
- libnode 从源码构建,在托管 runner 上以小时计,所以安装结果按 Node 版本 + `build_libnode.py` 内容缓存。tag 触发的运行只能读到默认分支上建立的缓存,由 `main` 上的运行预热;缓存 7 天无人访问会被回收,之后第一次运行会重新构建。
- 发布构建关闭 `MOMO_WARNINGS_AS_ERRORS`,不构建也不运行单元测试。
- Windows 任务用 Visual Studio 生成器;**该任务尚未在 GitHub runner 上运行验证**(macOS 的打包与冒烟已在本地验证)。
