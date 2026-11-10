# 10 · 编码规范

## 1. C++ 规范

基线:Google C++ Style Guide + Chromium/Flutter engine 习惯,关键差异与强调项如下。

**语言与特性**

- C++20;禁用异常(`-fno-exceptions`)与 RTTI(`-fno-rtti`)。不写 `throw/try/catch`,不用 `dynamic_cast`/`typeid`。
- 错误处理:可失败 API 返回 `momo::Status` / `momo::StatusOr<T>`(base/status.h);不可恢复错误 `MOMO_CHECK`(release 仍生效)/`MOMO_DCHECK`(debug)。构造函数不做可失败工作——用静态 `Create()` 返回 `StatusOr<unique_ptr<T>>`。
- 所有权:`std::unique_ptr` 默认;`shared_ptr` 仅限跨线程共享的不可变对象并在注释标注;裸指针/引用 = 非拥有借用,生命周期由调用方保证。禁 `new/delete` 裸用。
- 禁全局可变状态与静态初始化顺序依赖(常量 POD 除外)。
- 三方库边界:Skia/V8 类型不泄漏出所在模块的公共头(graphics 对 shell 暴露自己的类型;v8.h 只出现在 runtime 内部与 ui 绑定实现中)。

**命名与文件**

- 文件 `snake_case.{h,cc,mm}`;类型 `PascalCase`;方法 `PascalCase()`(Google 风格);成员变量 `trailing_underscore_`;常量 `kCamelCase`。
- 每文件一个主类;头文件 `#pragma once`;include 顺序:对应头 → C/C++ 标准库 → 三方 → 项目内。
- namespace:`momo::engine::graphics` 等与目录对应;不用 `using namespace`。

**线程与并发**

- 公共类注释声明线程约定;入口处 `MOMO_DCHECK_RUNS_ON(...)`。
- 有线程归属的类继承 `momo::Object`(base/object.h),入口处 `MOMO_DCHECK_ON_AFFINITY_THREAD()`;回调跨线程投递必须考虑接收对象已销毁:用 `WeakPtr<T>` + `PostToObject`;延迟/跨线程释放用 `ObjectPtr`/`DeleteLater`。
- 上层(engine/embedder)不直接使用 `std::thread`/`std::mutex`/`condition_variable`,通用并发设施一律放进 base(docs/12)。

**工具**

- clang-format(仓库根 `.clang-format`,Google 基线)+ clang-tidy(禁异常相关检查、bugprone、performance);两者进 CI。

## 2. TypeScript 规范

- strict 全开;eslint + prettier;禁 `any`(边界处用 `unknown` + 收窄)。
- 框架代码禁止引用 DOM/Node 全局(自定义 eslint 规则白名单 `momo:*`)。
- 公共 API 全部 TSDoc;对标 Flutter 的 API 文档密度。
- 类的私有成员用 `#field`;避免运行期装饰器(V8 snapshot 友好)。
- 性能敏感路径(layout/paint/事件分发)避免每帧分配:对象池、复用 TypedArray、避免闭包捕获在热循环中创建。

## 3. 提交与评审

- Conventional Commits(`feat(engine/graphics): ...`);每个 PR 对应 roadmap 任务编号。
- 公共接口变更必须同步更新 docs/ 对应文档——**文档即契约,代码与文档不一致视为 bug**。
