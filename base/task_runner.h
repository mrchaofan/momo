#pragma once

// TaskRunner: 向某一线程投递任务的句柄(docs/08 §2)。
// 线程约定: 所有方法线程安全, 可在任意线程调用。实例以 shared_ptr 共享。
// 目标线程已终止时投递的任务被丢弃(闭包在投递线程析构)。

#include <memory>

#include "base/closure.h"
#include "base/logging.h"
#include "base/time.h"

namespace momo {

class Object;

class TaskRunner {
 public:
  virtual ~TaskRunner() = default;

  virtual void PostTask(Closure task) = 0;
  virtual void PostDelayedTask(Closure task, TimeDelta delay) = 0;
  virtual bool RunsTasksOnCurrentThread() const = 0;

  // 安排 object 在本 runner 的线程上延迟析构(Object::DeleteLater 的实现入口, 业务代码不直接调用)。
  // EventLoop 的实现: 本轮迭代结束、栈上无任务时析构, 循环结束时必定清理。
  // 默认实现(测试替身等非 EventLoop 的 runner): 作为普通任务投递。
  virtual void ScheduleDeleteLater(Object* object);

  // 已在目标线程则同步执行, 否则投递。
  static void RunNowOrPostTask(TaskRunner& runner, Closure task) {
    if (runner.RunsTasksOnCurrentThread()) {
      task();
    } else {
      runner.PostTask(std::move(task));
    }
  }
};

}  // namespace momo

// debug 构建断言当前线程归属。runner 为指针或智能指针。
#define MOMO_DCHECK_RUNS_ON(runner) \
  MOMO_DCHECK((runner)->RunsTasksOnCurrentThread()) << "Called on the wrong thread. "
