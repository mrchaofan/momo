# 09 · 构建系统与依赖管理

## 1. 工具链

- CMake ≥ 3.24 + Ninja;Windows: clang-cl(与 Skia/V8 预编译产物 ABI 匹配性最好)或 MSVC;macOS: Xcode clang,最低部署 macOS 13.5(Node 26 / libnode 的要求)。
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
| rapidjson 或 simdjson | manifest/配置解析 | FetchContent(禁异常配置) |

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
momo::engine          (聚合 momo_engine_{platform,common,shell,runtime,ui,graphics,text,assets,extension})
momo_embedder_common  (静态库: 装配 + 平台抽象接口)
momo_embedder_{macos,windows}  (静态库: 平台实现, 只链接 momo_engine_platform)
momo_app_win / momo_app_mac   (可执行文件/App Bundle)
momo_ext_sdk          (INTERFACE: 扩展 C ABI 头)
```

可见性由 `target_link_libraries` 的 PRIVATE/PUBLIC 严格控制实现"engine 不见 embedder、平台实现只见 engine/platform 契约"。

## 4. JS/TS 构建

- framework/ 独立 npm workspace;框架不单独打包,应用 bundle 把 `@momo/framework` 直接解析到 `framework/packages/momo/src`(esbuild 选项在 `framework/scripts/common.mjs`)。
- 打应用 bundle 只有一个入口:`tools/momo-cli/momo.mjs bundle <app> [--out=<dir>]`,调 esbuild 产出 `<out>/bundle.js`,扫描依赖产出 `<out>/extensions/{extensions.manifest, 原生库}`(`<out>` 默认 `<app>/dist`;开发循环用它,再以 `Momo --dev --bundle=` 运行)。
- 应用打包不走 CMake,由 `tools/momo-cli/momo.mjs build` 完成:复制运行时,把 `momo bundle` 的产物与 `assets/` 放进:
  - macOS: `<Name>.app/Contents/Resources/{bundle.js, assets/, extensions/}`
  - Windows: `<Name>/{<Name>.exe, libnode.dll, bundle.js, assets/, extensions/}`(exe 同级即资源目录;`momo build --runtime=<含 Momo.exe 的目录>`, 未验证)
- CMake 只做一件相关的事:把 `MOMO_APP_DEFAULT_BUNDLE`(默认 `examples/hello_frame/main.js`)拷进 `Momo.app` 作为默认 bundle。
- startup snapshot(`tools/make_snapshot`)未实现,见 docs/04 §1。

## 5. 测试与 CI

- C++ 单元测试:gtest;engine 可在 headless(软件后端 + mock platform)下全量跑。
- 框架测试:同一组 `framework/test/*_test.ts` 既在 Node + mock `momo:ui` 下跑(`npm test`),也由 `momo_headless`(真实引擎 + 软件光栅)运行(CTest `FrameworkTests.EngineHeadless`)。
- Golden 像素测试:软件光栅化 PNG 比对(双平台跑同一组,保证一致性)。
- 跑测试的 CI 尚未建立(规划:win-x64 / mac-arm64 × debug(ASan)/ release;TSan 每日任务;发布流程见 §6,它不跑 `ctest`);ASan/TSan 构建可在本地用 `-DMOMO_ENABLE_ASAN=ON` / `-DMOMO_ENABLE_TSAN=ON` 生成(此时 V8 不可用, 见 §2.1)。

## 6. 打包与分发

`momo-cli build` 面向应用开发者:预编译的 engine 运行时 + 用户 JS/资源/扩展 → 产出 .app / 目录(后续 MSIX/DMG)。即应用开发者**无需本机编译 C++**,只有扩展作者需要工具链——这是 npm 式开发体验的关键。

### 6.1 发布产物

预编译运行时作为 GitHub Release 的附件发布,命名与布局参照 Electron(`<p>` 是 Node 的 `process.platform-process.arch`:`darwin-arm64`、`win32-x64`):

| 产物 | 内容 |
|---|---|
| `momo-v<version>-<p>.zip` | 运行时。macOS:`Momo.app`;Windows:`Momo.exe`、`libnode.dll`、`icudtl.dat`、`bundle.js`(默认 bundle)。另有 `version` 文件。解压后直接传给 `momo build --runtime=` |
| `momo-headless-v<version>-<p>.zip` | `momo_headless` + libnode:无窗口运行 bundle 的测试运行器 |
| `momo-ext-sdk-v<version>.zip` | 扩展 SDK:`include/`、`lib/cmake/MomoExtSdk/`、`types/`(与平台无关) |
| `momo-framework-v<version>.tgz` | `@momo/framework` 的 npm 包(与平台无关) |
| `SHASUMS256.txt` | 以上全部文件的 SHA256 |

- **版本号**只有一个来源:`framework/packages/momo/package.json` 的 `version`。CMake 读它(`MOMO_VERSION`,写进 `Momo.app` 的 Info.plist),打包脚本读它,发布 tag 必须是 `v<version>`。
- **包的内容**由 CMake 安装组件决定:`runtime`、`headless`(`embedder/CMakeLists.txt`)、`sdk`(`extensions/CMakeLists.txt`)。`tools/package_release.py` 对每个组件执行 `cmake --install --component` → ad-hoc 签名(macOS)→ 压缩,自己不描述布局。安装时 rpath 只保留包内路径(`@executable_path/../Frameworks`)。
- macOS 二进制是 **ad-hoc 签名**,没有 Developer ID 签名与公证;不带调试符号包。

```bash
cmake -S . -B out/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DMOMO_BUILD_TESTS=OFF -DMOMO_BUILD_EXAMPLES=OFF
cmake --build out/release
python3 tools/package_release.py --build-dir out/release --out out/dist --platform-independent
```

### 6.2 发布流程(`.github/workflows/release.yml`)

1. 把 `framework/packages/momo/package.json`(以及 workspace 根的 `framework/package.json`)的 `version` 改为新版本并合入 `main`。
2. 推送 tag `v<version>`(带 `-` 的版本,如 `v0.5.0-beta.1`,发为 prerelease)。
3. CI 在 `macos-15`(arm64)与 `windows-2022`(x64)上各自:校验 tag 与版本一致 → `fetch_deps.py` → Release 构建 → 打包 → **用解压出的 headless 包**跑 `embedder/headless/testdata/smoke.js`(验证包自带全部运行期依赖)。全部成功后汇总产物、生成 `SHASUMS256.txt`、创建 GitHub Release。

- 手动触发,或改动发布链路(workflow、`package_release.py`、`build_libnode.py`、`fetch_deps.py`、`cmake/`)的 PR 与 `main` 提交,走同一套构建与打包,但产物只作为 workflow artifact 上传,不创建 Release。
- libnode 从源码构建,在托管 runner 上以小时计,所以安装结果按 Node 版本 + `build_libnode.py` 内容缓存。tag 触发的运行只能读到默认分支上建立的缓存,由 `main` 上的运行预热;缓存 7 天无人访问会被回收,之后第一次运行会重新构建。
- 发布构建关闭 `MOMO_WARNINGS_AS_ERRORS`,不构建也不运行单元测试。
- Windows 任务用 Visual Studio 生成器;**该任务尚未在 GitHub runner 上运行验证**(macOS 的打包与冒烟已在本地验证)。
