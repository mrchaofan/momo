# 08 · 线程模型与任务调度

## 1. 四个 Task Runner

| Runner | 宿主线程 | 运行内容 | 禁止事项 |
|---|---|---|---|
| platform | OS 主线程(消息循环) | 平台 API、窗口/输入回调、扩展 on_attach | 长任务(>1ms 量级需转移) |
| ui | 引擎创建的专用线程 | V8/JS 全部执行、LayerTree 生成、Animator | 阻塞 IO、平台 API、GPU 调用 |
| raster | 引擎创建的专用线程 | GPU context、DisplayList 重放、present | 调用 JS、平台 API(present 除外,surface 实现内部处理) |
| io | 引擎创建的专用线程(+可扩为池) | 文件读取、图片解码、字体加载 | 调用 JS |

另有 `WorkerPool`(base):CPU 密集一次性任务(扩展可用),不保证顺序。

Node.js(docs/04 §1)另带几类线程,都不执行 JS、不碰 momo 对象:
- **uv poll 线程**("momo.uv.poll",每个 JS 运行时一个):只在 `uv_backend_fd` 上等待,有事件时向 ui 投递 `UvRunOnce`;
  与 ui 以 `uv_sem` 严格交替(Electron NodeBindings 的模型)。libuv 的回调全部在 ui 线程执行。
- **libuv 线程池**(fs、dns、zlib、crypto 等):完成后经 uv 循环回到 ui 线程。
- **Node 平台线程池**:V8 后台任务(并发编译、GC);V8 前台任务经 uv 句柄回到 ui 线程。
- **inspector IO 线程**(`--dev`):收发 DevTools 消息,经平台任务交给 ui 线程。

## 2. EventLoop、MessagePump 与 TaskRunner

**每个线程(含 OS 主线程)都运行同一种 `momo::EventLoop`**(base/event_loop.h,docs/12),差别只在 `MessagePump`(等待/唤醒策略):

| 线程 | EventLoop 的 pump | 创建者 |
|---|---|---|
| ui / raster / io | `DefaultMessagePump`(条件变量 + 定时等待) | `engine::ThreadHost` → `Thread` |
| platform(OS 主线程) | `momo::NativeMessagePump`:唤醒 = 向平台实现的 `NativeLoop` 投递"执行 DoWork"的原生任务;运行/退出 = NSApp run/stop、Win32 消息泵 | `embedder::Application::Create` |
| 测试 / headless | `DefaultMessagePump`,或 `RunUntilIdle()` 手动驱动 | 测试夹具 |

**一轮迭代(DoWork)**:

1. 取出"此刻已到期"任务的快照,按 (到期时间, 投递序号) 执行;执行期间新投递的任务进入下一轮(自我重投的任务不会饿死其他工作)。
2. **安全点**:若栈上已无任务(`task_depth() == 0`),析构所有被 `DeleteLater` 标记的对象(见 §3)。原生嵌套循环(任务内弹模态框、live resize)中 pump 会重入 DoWork:嵌套层只执行任务,不析构,直到最外层任务返回。
3. 返回下一次唤醒时刻,由 pump 等待。

**结束语义**:`Terminate()`(任意线程,幂等)之前已到期的任务全部执行;未到期的延迟任务与之后的投递被丢弃(闭包在循环线程析构);**结束时反复清理 DeleteLater 队列直到为空**,被标记的对象一定在本线程析构。结束后再 DeleteLater:在循环线程上立即析构,其他线程上记 ERROR 并泄漏(绝不在错误线程析构)。

**绑定**:每个循环只绑定一个线程,每个线程至多一个循环(`EventLoop::Current()`);`Run`/`RunUntilIdle` 不可嵌套。

```cpp
// base/task_runner.h —— 投递句柄, 以 shared_ptr 共享, 所有方法线程安全
class TaskRunner {
 public:
  virtual void PostTask(Closure task) = 0;          // Closure: 仅可移动的 void()
  virtual void PostDelayedTask(Closure task, TimeDelta delay) = 0;
  virtual bool RunsTasksOnCurrentThread() const = 0;
  virtual void ScheduleDeleteLater(Object* object); // Object::DeleteLater 的实现入口
};
```

`TaskRunners.platform` 即 platform EventLoop 的 runner;`NativeLoop` 只是 pump 的底层原语,engine 不直接使用。跨线程组合操作(`PostTaskAndWait`、`PostTaskAndReply[WithResult]`、`PostToObject`)见 base/task_util.h、base/object.h。

**所有权**:`ThreadHost` 由宿主持有(而非 Shell),因为 join 必须发生在 Shell 关闭**且窗口销毁之后**(docs/01 §4 第 4 步),只有宿主知道这个时点。契约:宿主保证线程比 Shell 活得久。

## 3. 对象的线程归属与生命周期(关键规则)

**`momo::Object`**(base/object.h,参考 QObject/NSObject)是需要线程归属的框架对象的基类(docs/12 §2):

- **归属(affinity)**:构造时绑定当前线程的 EventLoop(或显式指定 runner)。对象的方法只在归属线程调用;其他线程经 `PostToObject(WeakPtr<T>, fn)` 投递过去,在归属线程判活后再调用(`WeakPtr` 是项目内唯一的弱引用)。析构必须在归属线程。
- **`DeleteLater()`**:任意线程调用、幂等;对象在归属线程 EventLoop 的下一个安全点析构(§2),循环结束时必定清理。因此对象可以在自己的回调栈里被"删除",调用方之后仍可安全访问它直到返回事件循环。
- **所有权 `ObjectPtr<T>`**:独占(`unique_ptr` + 删除器),释放时不立即 delete 而是 `DeleteLater`;**不使用 shared_ptr 表达跨线程生命周期**。`MakeObject<T>(...)` 创建。归属线程上需要立即析构时可直接用 `std::unique_ptr`/`delete`。已被 `ObjectPtr` 持有的对象不得再手动 `DeleteLater`。
- **典型用例**:`platform::VsyncSource` 归属 platform 线程,由 `Shell` 以 `ObjectPtr` 持有;ui 线程的 `PlatformVsyncWaiter` 只持 `WeakPtr`,每次请求投递到 platform 线程判活后调用。解绑 = 释放 `ObjectPtr`(DeleteLater),ui 侧无需同步汇合。


| 对象 | 归属线程 | 跨线程方式 |
|---|---|---|
| v8::Isolate 及一切 V8 句柄 / Wrappable | ui | 绝不跨线程;数据以字节/POD 复制出入 |
| PlatformWindow/菜单/剪贴板 | platform | 经 PostTask 访问 |
| GrDirectContext/SkSurface/GPU 纹理 | raster | 绝不跨线程;包装为 raster 归属的 `Object`,释放即 DeleteLater |
| `platform::VsyncSource` | platform | `Object`;`ObjectPtr` 持有,他线程只持 `WeakPtr` 并经 `PostToObject` 调用 |
| `platform::RenderSurface` | 窗口(platform)拥有,raster 使用 | 唯一的跨线程使用例外:解绑时 `Shell::DetachView` 与 raster 同步汇合,之后窗口才销毁 |
| DisplayList/LayerTree/SkImage(已解码) | 不可变,shared_ptr | ui 产生 → raster 消费;最后引用可能在任一线程,GPU-backed 资源析构内部转投 raster |
| MessageChannelHub | 任意线程调用 | 内部加锁;JS handler 回调固定 ui 线程 |

同步原语:框架内部避免锁共享状态,优先"消息传递 + 不可变数据";必要处使用 `std::mutex` + 小临界区。提供 `WaitableEvent` 仅用于启动/关闭的有限几次汇合(如 Shell::Create 等待 ui/raster 就绪),帧路径上**零阻塞等待**(唯一例外:Windows resize 同步,docs/05 §5)。

## 4. 帧管线背压

见 docs/05 §5:渲染队列深度 1,raster 落后时 Animator 不再调度 JS 帧。`Pipeline<LayerTree>` 实现 produce/consume 计数,与 Flutter shell 的 pipeline 概念一致。

## 5. 关闭时序

严格逆序关停(docs/01 §4)。实现要点:每个 `EventLoop` 支持 `Terminate()`(§2 结束语义:排空已到期任务、丢弃其余、清理全部 DeleteLater 对象);platform EventLoop 由 `Application::Terminate` 请求结束,在当前任务返回后的安全点完成清理再退出原生循环(未进入消息循环时由 `~Application` 以 `RunUntilIdle` 收尾);`Shell::Shutdown()` 在 platform 线程同步完成 ui → raster 两侧的销毁;线程由宿主持有的 `ThreadHost` 拥有,在窗口销毁之后由 `ThreadHost::Join()` 按 ui → raster → io 顺序 join;扩展 on_detach 在 join 之前完成,确保没有扩展回调悬挂在队列里。

## 6. 调试支持

每线程命名(`momo.ui` 等,便于调试器/profiler);`MOMO_DCHECK_RUNS_ON(runner)` 宏在 debug 构建断言线程归属;TSan 配置纳入 CI(M2 起)。
