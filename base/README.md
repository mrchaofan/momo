# base/ — 全项目基础库

设计文档:`docs/12-base.md`。最底层,不依赖 engine/embedder;它们都依赖这里。命名空间 `momo`,CMake target `momo_base`(`momo::base`),单测 `momo_base_unittests`。

| 类别 | 文件 |
|---|---|
| 词汇类型 | `status.h`、`logging.h`、`macros.h`、`time.h`、`closure.h`、`geometry.h`、`matrix.h` |
| 对象模型 | `object.h/.cc`(Object、ObjectPtr、MakeObject、WeakPtr、PostToObject) |
| 事件循环 | `event_loop.h/.cc`、`message_pump.h/.cc` |
| 任务投递 | `task_runner.h/.cc`、`task_util.h/.cc`(PostTaskAndWait / PostTaskAndReply[WithResult]) |
| 线程 | `thread.h/.cc`、`worker_pool.h/.cc`、`waitable_event.h` |
| 并发数据结构 | `pipeline.h` |
| 原生循环适配 | `native_loop.h`(平台层实现)、`native_message_pump.h/.cc` |
| 测试支持 | `testing/scoped_test_event_loop.h`、`testing/mock_native_loop.h` |

规则:上层不直接使用 `std::thread`/`std::mutex`/`condition_variable`,不自造弱引用与任务投递;有线程归属的类继承 `Object`。
