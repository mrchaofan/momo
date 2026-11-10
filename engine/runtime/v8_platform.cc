#include "engine/runtime/v8_platform.h"

#include <chrono>
#include <utility>

#include "base/event_loop.h"
#include "base/logging.h"
#include "base/time.h"
#include "libplatform/libplatform.h"

namespace momo::engine::runtime {

namespace {

TimeDelta SecondsToDelta(double seconds) {
  return std::chrono::duration_cast<TimeDelta>(std::chrono::duration<double>(seconds));
}

}  // namespace

// ---------------------------------------------------------------------------
// ForegroundTaskRunner: v8::TaskRunner → momo::TaskRunner。

class V8Platform::ForegroundTaskRunner final : public v8::TaskRunner {
 public:
  ForegroundTaskRunner(std::shared_ptr<momo::TaskRunner> runner, std::function<void()> after_task)
      : runner_(std::move(runner)), state_(std::make_shared<State>()) {
    state_->after_task = std::move(after_task);
  }

  // 注销后已投递的任务不再执行(isolate 已销毁)。在 runner 线程调用。
  void Shutdown() {
    MOMO_DCHECK_RUNS_ON(runner_);
    state_->alive = false;
    state_->after_task = nullptr;
  }

  bool IdleTasksEnabled() override { return false; }
  bool NonNestableTasksEnabled() const override { return true; }
  bool NonNestableDelayedTasksEnabled() const override { return true; }

 protected:
  void PostTaskImpl(std::unique_ptr<v8::Task> task, const v8::SourceLocation&) override {
    Post(runner_, state_, std::move(task), TimeDelta::zero(), /*nestable=*/true);
  }
  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task, double delay_in_seconds,
                           const v8::SourceLocation&) override {
    Post(runner_, state_, std::move(task), SecondsToDelta(delay_in_seconds), /*nestable=*/true);
  }
  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation&) override {
    Post(runner_, state_, std::move(task), TimeDelta::zero(), /*nestable=*/false);
  }
  void PostNonNestableDelayedTaskImpl(std::unique_ptr<v8::Task> task, double delay_in_seconds,
                                      const v8::SourceLocation&) override {
    Post(runner_, state_, std::move(task), SecondsToDelta(delay_in_seconds), /*nestable=*/false);
  }

 private:
  // 仅 runner 线程读写(任务执行与 Shutdown 都在该线程)。
  struct State {
    bool alive = true;
    std::function<void()> after_task;  // 引用 JsRuntimeImpl, 注销时清空
  };

  // 不可嵌套任务若落在嵌套循环中(task_depth > 1, 当前任务自身算 1 层), 重新投递到外层再执行。
  // ui 线程的 EventLoop 实际不会嵌套, 这里只为满足 V8 的语义约束。
  static void Post(std::shared_ptr<momo::TaskRunner> runner, std::shared_ptr<State> state,
                   std::unique_ptr<v8::Task> task, TimeDelta delay, bool nestable) {
    momo::TaskRunner& target = *runner;
    target.PostDelayedTask(
        [runner = std::move(runner), state = std::move(state), task = std::move(task),
         nestable]() mutable {
          if (!state->alive) {
            return;
          }
          if (!nestable) {
            const EventLoop* loop = EventLoop::Current();
            if (loop != nullptr && loop->task_depth() > 1) {
              Post(std::move(runner), std::move(state), std::move(task), TimeDelta::zero(), false);
              return;
            }
          }
          task->Run();
          if (state->alive && state->after_task) {
            state->after_task();
          }
        },
        delay);
  }

  std::shared_ptr<momo::TaskRunner> runner_;
  std::shared_ptr<State> state_;
};

// ---------------------------------------------------------------------------
// V8Platform

V8Platform& V8Platform::Get() {
  static V8Platform* platform = new V8Platform();  // 有意泄漏, 见头文件
  return *platform;
}

V8Platform::V8Platform()
    : workers_("momo.v8"), tracing_controller_(std::make_unique<v8::TracingController>()) {}

void V8Platform::RegisterIsolate(v8::Isolate* isolate, std::shared_ptr<TaskRunner> runner,
                                 std::function<void()> after_task) {
  MOMO_CHECK(runner != nullptr);
  MOMO_DCHECK_RUNS_ON(runner);
  auto foreground = std::make_shared<ForegroundTaskRunner>(std::move(runner), std::move(after_task));
  std::lock_guard<std::mutex> lock(mutex_);
  const bool inserted = runners_.emplace(isolate, std::move(foreground)).second;
  MOMO_CHECK(inserted) << "Isolate registered twice";
}

void V8Platform::UnregisterIsolate(v8::Isolate* isolate) {
  std::shared_ptr<ForegroundTaskRunner> runner;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runners_.find(isolate);
    if (it == runners_.end()) {
      return;
    }
    runner = std::move(it->second);
    runners_.erase(it);
  }
  runner->Shutdown();
}

int V8Platform::NumberOfWorkerThreads() { return static_cast<int>(workers_.thread_count()); }

std::shared_ptr<v8::TaskRunner> V8Platform::GetForegroundTaskRunner(v8::Isolate* isolate,
                                                                    v8::TaskPriority /*priority*/) {
  // 优先级: ui 线程只有一个 FIFO 队列, 全部按投递顺序执行。
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = runners_.find(isolate);
  MOMO_CHECK(it != runners_.end()) << "V8 isolate is not registered with V8Platform";
  return it->second;
}

double V8Platform::MonotonicallyIncreasingTime() {
  return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

double V8Platform::CurrentClockTimeMillis() { return SystemClockTimeMillis(); }

v8::TracingController* V8Platform::GetTracingController() { return tracing_controller_.get(); }

std::unique_ptr<v8::JobHandle> V8Platform::CreateJobImpl(v8::TaskPriority priority,
                                                         std::unique_ptr<v8::JobTask> job_task,
                                                         const v8::SourceLocation& /*location*/) {
  // libplatform 的通用 Job 实现: 并发度调度建立在 PostTaskOnWorkerThread 之上。
  return v8::platform::NewDefaultJobHandle(this, priority, std::move(job_task),
                                           workers_.thread_count());
}

void V8Platform::PostTaskOnWorkerThreadImpl(v8::TaskPriority /*priority*/,
                                            std::unique_ptr<v8::Task> task,
                                            const v8::SourceLocation& /*location*/) {
  workers_.PostTask([task = std::move(task)] { task->Run(); });
}

void V8Platform::PostDelayedTaskOnWorkerThreadImpl(v8::TaskPriority /*priority*/,
                                                   std::unique_ptr<v8::Task> task,
                                                   double delay_in_seconds,
                                                   const v8::SourceLocation& /*location*/) {
  workers_.PostDelayedTask([task = std::move(task)] { task->Run(); },
                           SecondsToDelta(delay_in_seconds));
}

}  // namespace momo::engine::runtime
