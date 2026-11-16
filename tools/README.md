# tools/ — 工具链

| 工具 | 形式 | 状态 | 职责 |
|---|---|---|---|
| `fetch_deps.py` | Python 脚本 | ✅ | 按 `cmake/deps.lock.json` 并行分块下载、断点续传、SHA256 校验、安全解压预编译依赖;带 `build` 的依赖(libnode)转交对应脚本(`--force`/`--mirror`/`--jobs` 一并传下去) |
| `build_libnode.py` | Python 脚本 | ✅ | 从源码构建 libnode;源码包的下载、校验、平台键、戳记复用 `fetch_deps.py` |
| `package_release.py` | Python 脚本 | ✅ | 把 Release 构建打成发布产物(运行时 / headless / 扩展 SDK / 框架与 CLI 的 npm 包), 布局来自 CMake 安装组件;发布流程见 docs/09 §6 |
| `framework/packages/cli`(`@momo/cli`, 命令 `momo`) | npm 包 | ✅ | 不在本目录: 与 `@momo/framework` 同在 npm 工作区里, 随版本发布, 不依赖源码树。只做打包:`momo build <app-dir> --runtime=<Momo.app>`(复制运行时 → 选中的应用文件(`package.json`、`main`、`assets/**`, 加 `momo.include`, 去掉 `momo.ignore`)原样放进 `<resources>/app/`, 装配 `.app`,`--dmg` 出 DMG)。不编译、不分析文件内容:应用的源码怎么变成 `main` 由应用自己选工具 |
| `framework/scripts/build_builtin.mjs` | Node 脚本 | ✅ | 不在本目录: 用 esbuild 把运行时自带的 JS 模块(框架、内置扩展)编译到 `<out-dir>/<name>/`;由 CMake target `momo_builtin` 调用(docs/09 §4.1) |
| `embedder/headless`(`momo_headless`) | C++ 可执行 | ✅ | 无窗口运行一个应用(目录或 JS 文件;真实引擎 + 软件光栅), 经 `momo/test` 通道报告结果/截图;框架测试的引擎侧运行器 |
| `make_snapshot/` | C++ 小工具 | ⬜ M6 | 把框架 bundle 预编译进 V8 startup snapshot |

```bash
(cd examples/counter && npm run build)                                       # 示例自己的构建 → examples/counter/dist/main.js
node framework/packages/cli/momo.mjs build examples/counter --out=out/apps --runtime=out/debug/embedder/Momo.app
out/debug/embedder/momo_headless embedder/headless/testdata/smoke.js --screenshot=/tmp/a.png
```
