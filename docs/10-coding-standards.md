# 10 · 编码规范

只列真实在执行的规则：由编译选项、构建配置或评审保证。仓库里**没有**格式化与 lint 配置（没有 `.clang-format`、clang-tidy、eslint、prettier），也没有跑检查的 CI；新代码跟随所在文件的既有风格。线程、所有权、依赖方向这些"违反了会崩"的规则及其原因见 [ONBOARDING.md](../ONBOARDING.md) §4。

## 1. C++

- C++20；**禁用异常与 RTTI**（`-fno-exceptions -fno-rtti`，`cmake/momo_options.cmake`）；`-Wall -Wextra`，默认 warning-as-error。
- 错误处理：可失败 API 返回 `momo::Status` / `momo::StatusOr<T>`（`base/status.h`），用 `MOMO_RETURN_IF_ERROR` / `MOMO_ASSIGN_OR_RETURN` 传递；不可恢复错误 `MOMO_CHECK`（release 仍生效）/ `MOMO_DCHECK`（debug）。构造函数不做可失败的工作——用静态 `Create()` 返回 `StatusOr<unique_ptr<T>>`。
- 所有权：`std::unique_ptr` 默认；`shared_ptr` 只用于跨线程共享的不可变数据；有线程归属的对象继承 `momo::Object`，用 `ObjectPtr` / `WeakPtr` / `PostToObject`（ARCHITECTURE.md §2.1）；裸指针/引用 = 非拥有借用。
- 不引入全局可变状态与静态初始化顺序依赖。唯一的例外是 Node 的进程级初始化（`engine/runtime/js_runtime.cc`，只写一次、永不释放）。
- 三方库边界：Skia 头只出现在 `engine/graphics`、`text`、`assets` 的实现文件里；`v8.h` 只出现在 `engine/runtime`、`engine/ui` 的实现文件里。
- 并发原语：engine / embedder 不直接使用 `std::thread` / `std::mutex` / `condition_variable`，通用设施放进 `base/`。
- 命名与文件：文件 `snake_case.{h,cc,mm}`；类型与方法 `PascalCase`；成员变量 `trailing_underscore_`；常量 `kCamelCase`；头文件 `#pragma once`；以仓库根为 include 根（`#include "base/status.h"`）；命名空间与目录对应（`momo::engine::graphics`）。
- 每个头文件开头的注释写明该类的**线程约定**与**生命周期**，它是这类信息的唯一来源；有线程归属的类在方法入口 `MOMO_DCHECK_ON_AFFINITY_THREAD()`。

## 2. TypeScript

- `strict` 全开（`framework/tsconfig.json`，另有 `noUnusedLocals`、`noImplicitReturns`、`verbatimModuleSyntax`）；`npm run typecheck` 覆盖框架、测试与示例。
- 框架代码不引用 DOM / Node 全局：tsconfig 的 `lib` 只有 ES2022、`types` 为空，用到的少数全局在 `ui/globals.d.ts` 里声明。延迟任务走 `SchedulerBinding.scheduleDelayed`，不直接调 `setTimeout`（测试要能换假时钟）。
- 类的私有成员用 `#field`。
- 热路径（layout / paint / 事件分发）避免每帧分配：复用 TypedArray，不在热循环里创建闭包。

## 3. 提交与文档

- 提交信息用一句英文陈述改动的结果（看 `git log`）。
- 每类信息只在一个地方维护（清单见 ONBOARDING.md §8）。改了类名、文件名或流程，同步改 ARCHITECTURE.md 与头文件注释；文档里写的类名必须在代码中真实存在，没验证过的行为写"未验证"。
