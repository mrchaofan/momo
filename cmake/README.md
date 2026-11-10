# cmake/ — CMake 模块

| 文件 | 职责 |
|---|---|
| `momo_options.cmake` | 全局编译选项:C++20、`-fno-exceptions -fno-rtti`(MSVC: `/EHs-c- /GR-`)、警告级别、ASan/TSan 开关 |
| `momo_dependencies.cmake` | FetchContent 小型依赖(googletest 等) |
| `momo_prebuilt.cmake` | 预编译依赖:读取 `third_party/prebuilt/<name>/.momo_dep.json`,导出 `momo::skia`、`momo::v8` |
| `deps.lock.json` | 预编译包的版本、URL 与 SHA256(由 `tools/fetch_deps.py` 使用) |

工具链文件与 CMake 侧的应用打包没有做:应用打包由 `tools/momo-cli` 完成。

约定:所有库 target 命名 `momo_<layer>_<module>`(如 `momo_engine_graphics`),对外聚合 target 为 `momo::engine`。依赖策略详见 `docs/09-build-and-deps.md`。
