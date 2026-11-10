# framework/ — UI 框架层(TypeScript)

设计文档:`docs/06-framework-js.md`。运行在引擎 V8 中,发布为 npm 包 `@momo/framework`。

```
packages/momo/src/
├── ui/          # momo:ui 类型声明与薄包装(types.ts、window.ts、canvas.ts、paragraph.ts)
├── foundation/  # change_notifier.ts、key.ts、diagnostics.ts、assert.ts
├── scheduler/   # binding.ts(帧阶段)、ticker.ts、priority.ts
├── animation/   # controller.ts、tween.ts、curves.ts、simulation.ts
├── gestures/    # pointer_router.ts、arena.ts、recognizers/{tap,drag,scroll,...}.ts
├── painting/    # text_painter.ts、decoration.ts、image_provider.ts、borders.ts、colors.ts
├── rendering/   # object.ts(RenderObject/PipelineOwner)、box.ts、flex.ts、stack.ts、
│                #   padding.ts、paragraph.ts、image.ts、proxy_box.ts、view.ts、layer.ts
├── widgets/     # framework.ts(Widget/Element/BuildOwner)、binding.ts(runApp)、basic.ts、
│                #   container.ts、text.ts、image.ts、gesture_detector.ts、focus.ts、
│                #   scroll/*.ts、inherited.ts
├── material/    # (M6)button.ts、text_field.ts、dialog.ts、theme.ts
└── services/    # message_channel.ts、standard_codec.ts、clipboard.ts、menu.ts、
                 #   shortcuts.ts、lifecycle.ts、text_input.ts
test/            # headless 引擎驱动的框架测试 + golden
```

规则:禁 DOM/Node 全局(eslint 强制);热路径零分配约定见 `docs/10-coding-standards.md` §2。实现从 M4 开始,但 `ui/types.ts`(momo:ui 的 .d.ts)随 M1 的绑定同步维护,**它是 JS↔C++ 的接口契约文件**。

## 开发命令(M4 已实现)

```bash
cd framework
npm install                      # esbuild + typescript(devDependencies)
npx tsc --noEmit                 # 类型检查(strict;含 test/ 与 examples/counter、examples/todo)
npm test                         # 构建并运行全部测试(Node + mock momo:ui/momo:services)
node out/test/node_main.mjs <过滤子串>   # 只跑名称包含子串的测试(先 npm run build:tests)
npm run build                    # 框架单文件 bundle → out/momo_framework.js(momo:* 保持外部导入)

# 示例 → 单文件 ESM bundle(只保留 `import ... from "momo:*"`)
node scripts/build_example.mjs ../examples/counter/src/main.ts ../examples/counter/dist/main.js
node scripts/build_example.mjs ../examples/todo/src/main.ts    ../examples/todo/dist/main.js
#   可选 --minify / --no-sourcemap;npm run build:examples 一次构建两个示例
```

- 工作区:`framework/package.json` 为 npm workspace 根;发布包为 `packages/momo`(`@momo/framework`,
  `exports` 指向 TS 源码, 子路径 `@momo/framework/widgets` 等)。构建脚本把 `@momo/framework` 别名到源码。
- 测试布局:`test/*_test.ts` 写成与宿主无关的场景函数(`test/harness.ts` 极简运行器 + `test/tester.ts` WidgetTester,
  只通过 `window` 回调与 `defaultBinaryMessenger` 驱动框架);`test/mock/` 是 Node 宿主用的 momo:ui / momo:services mock
  (记录绘制与合成调用、Ahem 固定宽度字形模型)。`test/examples/*_main.ts` 在 mock 下端到端运行两个示例。
- 引擎宿主(`momo_headless`, CTest `FrameworkTests.EngineHeadless`):`npm run build:tests` 产出 `out/test/momo_tests.js`(momo:* 为外部导入), 用法:
  `import { runTests } from "./momo_tests.js"; const s = await runTests({ name: "engine-headless", log: print, ahemText: false, mock: null });`
  依赖 mock 观察能力或 Ahem 字形度量的用例会自动 skip。
