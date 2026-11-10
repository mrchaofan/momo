#pragma once

// ScopedTestEventLoop: 在当前(测试)线程上创建并绑定一个 EventLoop;析构时结束它
// (排空任务、清理 DeleteLater 对象)。用于需要 momo::Object 默认归属或手动驱动任务的测试。

#include <memory>

#include "base/event_loop.h"
#include "base/macros.h"
#include "base/task_runner.h"

namespace momo::testing {

class ScopedTestEventLoop {
 public:
  ScopedTestEventLoop() : loop_(EventLoop::Create()) { loop_->BindToCurrentThread(); }
  ~ScopedTestEventLoop() {
    loop_->Terminate();
    loop_->RunUntilIdle();
  }

  EventLoop& loop() { return *loop_; }
  std::shared_ptr<TaskRunner> task_runner() { return loop_->GetTaskRunner(); }
  // 执行已到期任务与延迟析构, 不等待延迟任务。
  size_t RunUntilIdle() { return loop_->RunUntilIdle(); }

 private:
  std::shared_ptr<EventLoop> loop_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(ScopedTestEventLoop);
};

}  // namespace momo::testing
