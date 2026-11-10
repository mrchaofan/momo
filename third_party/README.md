# third_party/ — 第三方依赖

本目录不入库源码,只存放依赖说明、补丁与接入冒烟测试。获取方式见 `docs/09-build-and-deps.md` §2(预编译包 + FetchContent),锁定文件为 `cmake/deps.lock.json`。

```bash
python3 tools/fetch_deps.py     # → third_party/prebuilt/{skia,v8}(已 gitignore), 下载缓存在 third_party/.downloads/
```

- `prebuilt/`:解压后的预编译包(含 `.momo_dep.json` 戳记),由 `cmake/momo_prebuilt.cmake` 导出 `momo::skia`、`momo::v8`。
- `smoke_test/`:`momo_thirdparty_smoketest`,验证头文件、配置宏、链接与运行期(rpath、ICU 数据)。

当前版本:Skia m144(JetBrains skia-pack)、V8 15.8.62.9(Homebrew bottle, dylib)。限制见 docs/09 §2.1。

| 依赖 | 版本策略 | 形式 |
|---|---|---|
| Skia (+skparagraph/skshaper/skunicode) | 跟随 chrome/mXXX 稳定分支,季度评估 | 预编译 + IMPORTED target |
| V8 (monolith) | 稳定分支,与 Skia 同一 ICU | 预编译 + IMPORTED target |
| ICU | 由 Skia/V8 包统一提供 | 预编译 |
| ANGLE | Windows only | 预编译 |
| spdlog / googletest / rapidjson | 固定 tag | FetchContent |

`patches/` 子目录(需要时建立)存放对预编译脚本或源码构建的补丁,每个补丁附上游 issue 链接。
