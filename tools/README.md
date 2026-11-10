# tools/ — 工具链

| 工具 | 形式 | 状态 | 职责 |
|---|---|---|---|
| `fetch_deps.py` | Python 脚本 | ✅ | 按 `cmake/deps.lock.json` 并行分块下载、断点续传、SHA256 校验、安全解压预编译依赖 |
| `momo-cli/momo.mjs` | Node CLI | ✅ | `momo build <app>`(esbuild 打包 + 依赖中扩展声明 → `extensions.manifest` + 原生库拷贝 + 装配 `.app`,`--dmg` 出 DMG)、`momo manifest`、`momo ext create/verify` |
| `embedder/headless`(`momo_headless`) | C++ 可执行 | ✅ | 无窗口运行 bundle(真实引擎 + 软件光栅), 经 `momo/test` 通道报告结果/截图;框架测试的引擎侧运行器 |
| `make_snapshot/` | C++ 小工具 | ⬜ M6 | 把框架 bundle 预编译进 V8 startup snapshot |

```bash
node tools/momo-cli/momo.mjs build examples/counter --out=out/apps
node tools/momo-cli/momo.mjs ext verify out/debug/extensions/examples/hello-extension/native/libext_hello.dylib
out/debug/embedder/momo_headless --bundle=embedder/headless/testdata/smoke.js --screenshot=/tmp/a.png
```
