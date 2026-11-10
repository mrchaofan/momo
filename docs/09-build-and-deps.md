# 09 · 构建系统与依赖管理

## 1. 工具链

- CMake ≥ 3.24 + Ninja;Windows: clang-cl(与 Skia/V8 预编译产物 ABI 匹配性最好)或 MSVC;macOS: Xcode clang,最低部署 macOS 12。
- C++20;全局 `-fno-exceptions -fno-rtti`(MSVC `/EHs-c- /GR-`),warning-as-error。
- 尚未提供 `CMakePresets.json` 与 CI(规划:`win-x64-*`、`mac-arm64-*`、`headless-test`),目前直接用 `cmake -S . -B out/debug -G Ninja`。

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
| ANGLE | Windows GL→D3D11 | 预编译包(可取自 Chromium 发布或自建) |

所有预编译包带 SHA256 锁定在 `cmake/deps.lock.json`;`tools/fetch_deps.py` 支持离线镜像。

### 2.1 当前落地(macOS arm64)

| 依赖 | 版本 | 来源 | 形态 | 备注 |
|---|---|---|---|---|
| Skia | m144-e2e6623374-4 | JetBrains skia-pack release | 静态库(skia、skparagraph、skshaper、skunicode_icu、harfbuzz、icu…) | 系统 libc++(`std::__1`);ICU 符号带 `_skiko` 后缀, 不与 V8 的 ICU 冲突;含 Metal/GL 后端 |
| V8 | 15.8.62.9 | Homebrew bottle(ghcr.io) | component 构建: `libv8*.dylib` 等 10 个 dylib + `icudtl.dat` | `use_custom_libcxx=false`(系统 libc++, 与我们 ABI 一致);pointer compression 开、sandbox 关 → 嵌入方需定义 `V8_COMPRESS_POINTERS`、`V8_31BIT_SMIS_ON_64BIT_ARCH` |

与原规划的差异与已知限制:

- **V8 是 dylib 而非 monolith 静态库**。`fetch_deps.py` 解压后把 install name 改为 `@rpath/...` 并 ad-hoc 重签名;打包 `.app` 时需把 dylib 拷进 `Contents/Frameworks`、`icudtl.dat` 拷进 `Contents/Resources`(M1 接入 runtime 时实现)。
- **V8 dylib 要求 macOS ≥ 13**,高于项目的部署目标 12(链接时有警告)。要么把最低系统提到 13,要么换自建的 V8 包。
- **V8 不能用于 ASan/TSan 构建**:包内 partition_alloc 的 malloc shim 与 sanitizer 冲突。sanitizer 构建中 `momo::v8` 不可用(`cmake/momo_prebuilt.cmake` 自动关闭)。
- Windows:Skia 已锁定 `windows-x64` 包;V8 尚无 Windows 预编译来源(需自建 CI 产出)。

### 2.2 获取与接入

```bash
python3 tools/fetch_deps.py            # 当前平台全部依赖 → third_party/prebuilt/{skia,v8}
```

下载器按块并行发 Range 请求(很多代理对单连接限速),支持断点续传,并做 SHA256 校验;解压前检查路径穿越,先解到全新空目录再整体替换。`cmake/momo_prebuilt.cmake` 根据 `third_party/prebuilt/<name>/.momo_dep.json` 戳记导出 `momo::skia`、`momo::v8`;缺包时只提示、不报错(`-DMOMO_REQUIRE_PREBUILT=ON` 时缺包即报错)。接入是否正确由 `momo_thirdparty_smoketest` 验证:Skia 光栅绘制与段落排版、V8 执行脚本与 Intl。

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

- framework/ 独立 npm workspace;`npm run build` 用 esbuild 产出 `framework/out/momo_framework.js`(ESM, 单文件)。
- 应用打包不走 CMake,由 `tools/momo-cli/momo.mjs build` 完成:调 esbuild 打应用 bundle,扫描依赖产出 `extensions.manifest`,拷贝进:
  - macOS: `<Name>.app/Contents/Resources/{bundle.js, assets/, extensions/}`
  - Windows: `<name>/{bundle.js, assets/, extensions/}`(exe 同级,未验证)
- CMake 只做一件相关的事:把 `MOMO_APP_DEFAULT_BUNDLE`(默认 `examples/hello_frame/main.js`)拷进 `Momo.app` 作为默认 bundle。
- startup snapshot(`tools/make_snapshot`)未实现,见 docs/04 §1。

## 5. 测试与 CI

- C++ 单元测试:gtest;engine 可在 headless(软件后端 + mock platform)下全量跑。
- 框架测试:同一组 `framework/test/*_test.ts` 既在 Node + mock `momo:ui` 下跑(`npm test`),也由 `momo_headless`(真实引擎 + 软件光栅)运行(CTest `FrameworkTests.EngineHeadless`)。
- Golden 像素测试:软件光栅化 PNG 比对(双平台跑同一组,保证一致性)。
- CI 尚未建立(规划:win-x64 / mac-arm64 × debug(ASan)/ release;TSan 每日任务);ASan/TSan 构建可在本地用 `-DMOMO_ENABLE_ASAN=ON` / `-DMOMO_ENABLE_TSAN=ON` 生成(此时 V8 不可用, 见 §2.1)。

## 6. 打包与分发

`momo-cli build` 面向应用开发者:拉预编译的 engine 运行时 + 用户 JS/资源/扩展 → 产出 .app / 目录(后续 MSIX/DMG)。即应用开发者**无需本机编译 C++**,只有扩展作者需要工具链——这是 npm 式开发体验的关键。
