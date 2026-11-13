# examples/ — 示例应用

只保留有实际用途的三个示例（每个都被构建或测试使用）：

| 示例 | 层次 | 用途 |
|---|---|---|
| `hello_frame/` | 底层 `momo:ui` | 不用框架直接画动画帧（渐变卡片、旋转星形、描边）。CMake 把它作为默认 `bundle.js` 打包进 `Momo.app`；`ShellTest` 用它验证 bundle 执行 |
| `counter/` | View | Material 计数器：按钮、主题、键盘快捷键。快速开始示例；`framework/test/examples/counter_main.ts` 端到端跑它 |
| `todo/` | View | 输入框、列表、勾选、删除、滚动。`framework/test/examples/todo_main.ts` 端到端跑它（覆盖 ListTile、单行输入框回车提交、带 key 的列表删除等单元测试没有的路径） |

底层 API 示例无需构建，直接运行（`--dev` 下修改文件即 hot restart）：

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev --bundle=examples/hello_frame/main.js
```

View 示例是独立 npm 工程，依赖 `@momo/framework`：

```bash
node framework/scripts/build_example.mjs examples/counter/src/main.ts examples/counter/dist/main.js
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev --bundle=examples/counter/dist/main.js
node tools/momo-cli/momo.mjs build examples/counter --out=out/apps     # 打包成 .app
```

扩展的使用示例见 `extensions/examples/`（hello、texture）。
