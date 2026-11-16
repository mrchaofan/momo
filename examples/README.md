# examples/ — 示例应用

只保留有实际用途的示例（每个都被构建或测试使用）：

| 示例 | 层次 | 用途 |
|---|---|---|
| `hello_frame/` | 底层 `momo:ui` | 不用框架直接画动画帧（渐变卡片、旋转星形、描边）。CMake 把它作为默认应用打包进 `Momo.app`（`Resources/app/`）；`ShellTest` 用它验证 bundle 执行 |
| `counter/` | View | Material 计数器：按钮、主题、键盘快捷键。快速开始示例；`framework/test/examples/counter_main.ts` 端到端跑它 |
| `todo/` | View | 输入框、列表、勾选、删除、滚动。`framework/test/examples/todo_main.ts` 端到端跑它（覆盖 ListTile、单行输入框回车提交、带 key 的列表删除等单元测试没有的路径） |
| `webview/` | View + 内置扩展 `momo:webview` | 多窗口、多标签浏览器（macOS）；应用菜单（网页里的剪切/拷贝/粘贴、页内查找、全部快捷键）。`framework/test/examples/browser_main.ts` 端到端跑它（原生插件换成替身） |

底层 API 示例无需构建，直接运行：

```bash
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev examples/hello_frame
```

View 示例不依赖任何 npm 包，不需要 `npm install`：框架（`momo:framework`）与内置扩展（`momo:webview`）由运行时提供，`dist/main.js` 里只有示例自己的代码。`npm run build` 是一行 esbuild（`momo:*` 的导入原样保留；借用 `framework/node_modules` 里的 esbuild，所以要先在 `framework/` 里 `npm install`）：

```bash
(cd examples/counter && npm run build)                                        # src/main.ts → examples/counter/dist/main.js
out/debug/embedder/Momo.app/Contents/MacOS/Momo --dev examples/counter
node framework/packages/cli/momo.mjs build examples/counter --out=out/apps --runtime=out/debug/embedder/Momo.app   # 打包成 .app（只复制文件，先 build）
```

类型检查在 `framework/` 里做（`npx tsc --noEmit`；`framework/tsconfig.json` 的 `paths` 把 `momo:framework`、`momo:webview` 指到源码）。三方扩展长什么样见 `extensions/examples/`（hello、texture）。
