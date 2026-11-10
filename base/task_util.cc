#include "base/task_util.h"

#include "base/waitable_event.h"

namespace momo {

namespace {

// 随任务一起投递的"完成信号": 无论任务被执行还是被丢弃(闭包析构), 都会唤醒等待方。
class CompletionSignal {
 public:
  CompletionSignal(WaitableEvent* event, bool* ran) : event_(event), ran_(ran) {}
  ~CompletionSignal() { event_->Signal(); }
  void MarkRan() { *ran_ = true; }

 private:
  WaitableEvent* event_;
  bool* ran_;
};

}  // namespace

bool PostTaskAndWait(TaskRunner& runner, Closure task) {
  if (runner.RunsTasksOnCurrentThread()) {
    task();
    return true;
  }
  WaitableEvent done;
  bool ran = false;
  runner.PostTask([signal = std::make_unique<CompletionSignal>(&done, &ran),
                   task = std::move(task)]() mutable {
    task();
    signal->MarkRan();
    signal.reset();  // 先于闭包其余部分析构: 执行完立即唤醒
  });
  done.Wait();
  return ran;
}

}  // namespace momo
