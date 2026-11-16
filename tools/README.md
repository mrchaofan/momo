# tools/ — 工具链

| 工具 | 形式 | 状态 | 职责 |
|---|---|---|---|
| `fetch_deps.py` | Python 脚本 | ✅ | 按 `cmake/deps.lock.json` 并行分块下载、断点续传、SHA256 校验、安全解压预编译依赖;带 `build` 的依赖(libnode)转交对应脚本(`--force`/`--mirror`/`--jobs` 一并传下去) |
| `build_libnode.py` | Python 脚本 | ✅ | 从源码构建 libnode;源码包的下载、校验、平台键、戳记复用 `fetch_deps.py` |
| `package_release.py` | Python 脚本 | ✅ | 把 Release 构建打成发布产物(运行时 / headless / 扩展 SDK / 框架与 CLI 的 npm 包), 布局来自 CMake 安装组件;发布流程见 docs/09 §6 |
| `framework/packages/cli`(`@momo/cli`, 命令 `momo`) | npm 包 | ✅ | 不在本目录: 与 `@momo/framework` 同在 npm 工作区里, 随版本发布, 不依赖源码树。`momo bundle <app>`(打应用 bundle 的唯一入口:esbuild 打包 → `bundle.js`,代码里按路径 import 的原生扩展 `.node` → `native/`)、`momo build <app> --runtime=<Momo.app>`(`bundle` 的产物 + `assets/` + 运行时 → 装配 `.app`,`--dmg` 出 DMG)、`momo ext create`(原生扩展脚手架) |
| `embedder/headless`(`momo_headless`) | C++ 可执行 | ✅ | 无窗口运行 bundle(真实引擎 + 软件光栅), 经 `momo/test` 通道报告结果/截图;框架测试的引擎侧运行器 |
| `make_snapshot/` | C++ 小工具 | ⬜ M6 | 把框架 bundle 预编译进 V8 startup snapshot |

```bash
node framework/packages/cli/momo.mjs bundle examples/counter                 # → examples/counter/dist/bundle.js
node framework/packages/cli/momo.mjs build examples/counter --out=out/apps --runtime=out/debug/embedder/Momo.app
node framework/packages/cli/momo.mjs ext create my-ext                       # 原生扩展脚手架(Node 插件 + JS 门面)
out/debug/embedder/momo_headless --bundle=embedder/headless/testdata/smoke.js --screenshot=/tmp/a.png
```
