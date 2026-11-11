# 12 · base 基础库

`base/` 是全项目的最底层:platform、engine、embedder(以及将来的扩展宿主)都依赖它,它不依赖任何上层。对标 Chromium `base`、Qt Core 的对象与事件循环部分。命名空间为 `momo`(这些是全项目共用的词汇类型),CMake target `momo_base`(别名 `momo::base`),单测 `momo_base_unittests`。

**原则**:engine/embedder/platform 中不出现裸线程原语(`std::thread`、`std::mutex`、`condition_variable`)与自造的弱引用/任务投递;需要时先在 base 中提炼通用设施。

## 1. 内容一览

| 类别 | 文件 | 说明 |
|---|---|---|
| 词汇类型 | `status.h`、`logging.h`、`macros.h`、`time.h`、`closure.h`、`geometry.h`、`matrix.h` | Status/StatusOr 与 `MOMO_RETURN_IF_ERROR`;`MOMO_LOG/CHECK/DCHECK`;仅可移动的 `Closure`;Point/Size/Rect/Matrix |
| 对象模型 | `object.h/.cc` | `Object`、`ObjectPtr<T>`、`MakeObject<T>()`、`WeakPtr<T>`、`PostToObject()`、`MOMO_DCHECK_ON_AFFINITY_THREAD()` |
| 事件循环 | `event_loop.h/.cc`、`message_pump.h/.cc` | 统一 `EventLoop`(任务调度 + 延迟析构)与可替换的 `MessagePump`(默认:条件变量) |
| 任务投递 | `task_runner.h/.cc`、`task_util.h/.cc` | `TaskRunner`;`PostTaskAndWait`、`PostTaskAndReply`、`PostTaskAndReplyWithResult` |
| 线程 | `thread.h/.cc`、`worker_pool.h/.cc`、`waitable_event.h` | 具名线程(自带 EventLoop)、CPU 线程池、启动/关闭用的汇合事件 |
| 并发数据结构 | `pipeline.h` | 固定深度生产/消费管道(帧管线背压) |
| 原生循环适配 | `native_loop.h`、`native_message_pump.h/.cc` | 平台层实现 `NativeLoop`(NSApp/GCD、Win32 泵),`NativeMessagePump` 把它适配成 EventLoop 的 pump |
| 测试支持 | `testing/scoped_test_event_loop.h`、`testing/mock_native_loop.h` | 测试线程上的临时 EventLoop;可嵌套的模拟原生循环 |

## 2. 对象模型:Object

参考 QObject/NSObject。**有线程归属的框架对象都继承 `momo::Object`**(Application、WindowController、Shell、Engine、Animator、VsyncWaiter、Rasterizer、平台的 VsyncSource…)。

- **归属(affinity)**:构造时绑定当前线程的 `EventLoop::Current()`(或显式传入 runner)。方法只在归属线程调用(`MOMO_DCHECK_ON_AFFINITY_THREAD()`);他线程经 `PostToObject(weak, fn)` 投递过去、判活后执行。析构必须在归属线程。
- **`DeleteLater()`**:任意线程调用、幂等;对象在归属线程 EventLoop 的下一个安全点析构(§3),循环结束时必定清理。对象可以在自己的回调栈里被"删除"。
- **所有权**:`ObjectPtr<T>`(独占;释放 = DeleteLater)。不用 `shared_ptr` 表达跨线程生命周期。归属线程上需要立即析构时用 `std::unique_ptr` / 成员对象即可。已被 `ObjectPtr` 持有的对象不要再手动 DeleteLater。
- **弱引用**:`WeakPtr<T>(obj)`,项目内唯一的弱引用机制。可随任务投递到任意线程,只能在归属线程解引用;对象析构或调用 `InvalidateWeakPtrs()`(如"关闭"时)后失效。

## 3. 线程与事件循环

所有线程运行同一种 `EventLoop`,差别只在 pump:

| 线程 | pump | 创建者 |
|---|---|---|
| ui / raster / io(及 `Thread`) | `DefaultMessagePump` | `engine::ThreadHost` → `momo::Thread` |
| OS 主线程(platform) | `NativeMessagePump(platform.native_loop())` | `embedder::Application::Create` |
| 测试 | `RunUntilIdle()` 手动驱动 | `testing::ScopedTestEventLoop` |

**一轮迭代**:取出此刻已到期任务的快照依次执行(期间新投递的进入下一轮)→ 若栈上无任务(非嵌套),析构所有 DeleteLater 对象(**安全点**)→ 返回下一次唤醒时刻。原生嵌套循环(模态框、live resize)中 pump 重入 DoWork:嵌套层只执行任务、不析构。宿主在原生同步等待里(如拖拽缩放时主线程等新尺寸的帧)可调用 `RunDueTasksNested()` 执行一轮已到期任务,语义相同(不析构、不结束循环)。

**结束**:`Terminate()` 之前已到期的任务全部执行;其余丢弃(闭包在循环线程析构);反复清理 DeleteLater 队列直到为空。结束后 DeleteLater:循环线程上立即析构,他线程上记 ERROR 并泄漏(绝不在错误线程析构)。

**绑定**:每个循环绑定一个线程,每个线程至多一个循环;`Run`/`RunUntilIdle` 不可嵌套。

**任务组合(task_util.h)**:`PostTaskAndWait` 仅用于启动/关闭汇合点(目标线程已终止时返回 false,不死锁);`PostTaskAndReply[WithResult]` 用于"在 A 线程做事、回 B 线程汇报"(如 `Shell::RequestClose`:ui 上询问 JS,platform 上回复)。

## 4. 依赖方向

```
embedder ──► engine(含 engine/platform 契约)──► base
   └────────────────────────────────────────────► base
```

engine/platform 依赖 base(`VsyncSource` 继承 `Object`);embedder 的 `PlatformIntegration::native_loop()` 返回 base 的 `NativeLoop`。base 不依赖任何上层,无循环。
