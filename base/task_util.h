#pragma once

// 跨线程任务的常用组合(docs/12 §3)。都建立在 TaskRunner::PostTask 之上, 任意线程可调用。
//
//   PostTaskAndWait(runner, task)        在 runner 线程执行 task 并阻塞等待(仅限启动/关闭汇合点)。
//   PostTaskAndReply(runner, task, reply_runner, reply)
//                                        task 在 runner 线程执行, 完成后 reply 在 reply_runner 线程执行。
//   PostTaskAndReplyWithResult(...)      同上, task 的返回值传给 reply。
//
// 目标线程已终止时任务被丢弃: PostTaskAndWait 返回 false 而不会死锁;Reply 系列不会回复。

#include <memory>
#include <type_traits>
#include <utility>

#include "base/closure.h"
#include "base/task_runner.h"

namespace momo {

// 已在 runner 线程则直接执行。帧路径等热路径禁止使用(docs/08 §3)。返回 task 是否被执行。
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

template <typename Task, typename Reply>
void PostTaskAndReply(TaskRunner& runner, Task task, std::shared_ptr<TaskRunner> reply_runner,
                      Reply reply) {
  static_assert(std::is_void_v<decltype(task())>, "use PostTaskAndReplyWithResult");
  runner.PostTask([task = std::move(task), reply_runner = std::move(reply_runner),
                   reply = std::move(reply)]() mutable {
    task();
    reply_runner->PostTask(std::move(reply));
  });
}

}  // namespace momo
