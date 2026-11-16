#pragma once

// 跨线程任务的常用组合(ARCHITECTURE.md §2.4)。都建立在 TaskRunner::PostTask 之上, 任意线程可调用。
//
//   PostTaskAndWait(runner, task)        在 runner 线程执行 task 并阻塞等待(仅限启动/关闭汇合点)。
//   PostTaskAndReplyWithResult(runner, task, reply_runner, reply)
//                                        task 在 runner 线程执行, 返回值交给 reply_runner 线程上的 reply。
//
// 目标线程已终止时任务被丢弃: PostTaskAndWait 返回 false 而不会死锁;PostTaskAndReplyWithResult 不会回复。

#include <memory>
#include <type_traits>
#include <utility>

#include "base/closure.h"
#include "base/task_runner.h"

namespace momo {

// 已在 runner 线程则直接执行。帧路径等热路径禁止使用(ARCHITECTURE.md §1.3)。返回 task 是否被执行。
bool PostTaskAndWait(TaskRunner& runner, Closure task);

template <typename Task, typename Reply>
void PostTaskAndReplyWithResult(TaskRunner& runner, Task task,
                                std::shared_ptr<TaskRunner> reply_runner, Reply reply) {
  runner.PostTask([task = std::move(task), reply_runner = std::move(reply_runner),
                   reply = std::move(reply)]() mutable {
    auto result = task();
    reply_runner->PostTask([reply = std::move(reply), result = std::move(result)]() mutable {
      reply(std::move(result));
    });
  });
}

}  // namespace momo
