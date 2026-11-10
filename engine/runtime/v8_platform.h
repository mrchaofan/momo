#pragma once

// V8Platform: 把 V8 的任务调度接到 momo 的统一线程模型上(docs/04 §1、docs/08)。只能被
// engine/runtime 的实现文件包含。
//  - 前台任务(GetForegroundTaskRunner): 投递到 isolate 归属线程(ui)的 EventLoop, 与其他 ui 任务
//    同队列、同顺序执行;每个任务执行后排空 microtask(FinalizationRegistry 清理等会运行 JS)。
//    取代 libplatform 默认实现 —— 后者的前台队列只能靠 PumpMessageLoop 手动泵, 不泵就永远不执行。
//  - 后台任务 / Job: 由本平台独占的 base::WorkerPool("momo.v8.worker.*")执行。
//  - 时钟: base 的单调时钟。不支持 idle task。
//
// 线程约定: Get() 任意线程;RegisterIsolate/UnregisterIsolate 在 isolate 归属线程;
// v8::Platform 接口方法按 V8 要求线程安全。进程级单例, 有意泄漏(V8 不可 Dispose 后重建)。

#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "base/macros.h"
#include "base/task_runner.h"
#include "base/worker_pool.h"
#include "v8-platform.h"

namespace momo::engine::runtime {

class V8Platform final : public v8::Platform {
 public:
  // 首次调用时创建。
  static V8Platform& Get();

  // isolate 的前台任务投递到 runner。必须在 Isolate::Initialize 之前注册(V8 初始化期间即会取 runner),
  // 在 Isolate::Dispose 之后注销;注销后尚未执行的任务被丢弃(在 runner 线程析构)。
  // after_task 在每个前台任务执行后调用(用于排空 microtask), 可为空。
  void RegisterIsolate(v8::Isolate* isolate, std::shared_ptr<TaskRunner> runner,
                       std::function<void()> after_task);
  void UnregisterIsolate(v8::Isolate* isolate);

  // v8::Platform
  int NumberOfWorkerThreads() override;
  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(v8::Isolate* isolate,
                                                          v8::TaskPriority priority) override;
  double MonotonicallyIncreasingTime() override;
  double CurrentClockTimeMillis() override;
  v8::TracingController* GetTracingController() override;

 protected:
  std::unique_ptr<v8::JobHandle> CreateJobImpl(v8::TaskPriority priority,
                                               std::unique_ptr<v8::JobTask> job_task,
                                               const v8::SourceLocation& location) override;
  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority, std::unique_ptr<v8::Task> task,
                                  const v8::SourceLocation& location) override;
  void PostDelayedTaskOnWorkerThreadImpl(v8::TaskPriority priority, std::unique_ptr<v8::Task> task,
                                         double delay_in_seconds,
                                         const v8::SourceLocation& location) override;

 private:
  class ForegroundTaskRunner;

  V8Platform();

  WorkerPool workers_;
  std::unique_ptr<v8::TracingController> tracing_controller_;

  std::mutex mutex_;
  std::map<v8::Isolate*, std::shared_ptr<ForegroundTaskRunner>> runners_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(V8Platform);
};

}  // namespace momo::engine::runtime
