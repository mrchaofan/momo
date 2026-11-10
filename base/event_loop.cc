#include "base/event_loop.h"

#include <utility>

#include "base/logging.h"
#include "base/object.h"

namespace momo {

namespace {

thread_local EventLoop* g_current_loop = nullptr;

class EventLoopTaskRunner final : public TaskRunner {
 public:
  explicit EventLoopTaskRunner(std::shared_ptr<EventLoop> loop) : loop_(std::move(loop)) {}

  void PostTask(Closure task) override { loop_->PostTask(std::move(task), Clock::now()); }
  void PostDelayedTask(Closure task, TimeDelta delay) override {
    loop_->PostTask(std::move(task), Clock::now() + delay);
  }
  bool RunsTasksOnCurrentThread() const override { return loop_->RunsTasksOnCurrentThread(); }
  void ScheduleDeleteLater(Object* object) override { loop_->ScheduleDeleteLater(object); }

 private:
  std::shared_ptr<EventLoop> loop_;
};

}  // namespace

// ---------------------------------------------------------------------------
// 创建、绑定

std::shared_ptr<EventLoop> EventLoop::Create(std::unique_ptr<MessagePump> pump) {
  if (pump == nullptr) {
    pump = CreateDefaultMessagePump();
  }
  return std::shared_ptr<EventLoop>(new EventLoop(std::move(pump)));  // NOLINT: 私有构造
}

// pump 的 delegate 在循环线程上注册(BindToCurrentThread), 而不是在构造(创建者)线程:
// pump 回调 DoWork 的线程必须与注册它的线程相同(message_pump.h)。
EventLoop::EventLoop(std::unique_ptr<MessagePump> pump) : pump_(std::move(pump)) {}

EventLoop::~EventLoop() {
  // 被标记的对象持有本循环的 runner(即循环的共享引用), 所以走到这里时队列必然已清空。
  MOMO_DCHECK(deferred_deletes_.empty());
  if (g_current_loop == this) {
    g_current_loop = nullptr;
  }
}

EventLoop* EventLoop::Current() { return g_current_loop; }

void EventLoop::BindToCurrentThread() {
  bool has_work = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::thread::id self = std::this_thread::get_id();
    if (bound_) {
      MOMO_CHECK(thread_id_ == self) << "EventLoop is bound to another thread";
      return;
    }
    MOMO_CHECK(g_current_loop == nullptr) << "This thread already has an EventLoop";
    MOMO_CHECK(!exited_);
    bound_ = true;
    thread_id_ = self;
    g_current_loop = this;
    has_work = !queue_.empty() || !deferred_deletes_.empty();
    if (has_work) {
      next_wake_ = TimePoint::min();  // 绑定前的唤醒请求可能因 pump 尚无 delegate 而被丢弃: 重新请求
    }
  }
  pump_->Attach(this);  // 在循环线程上注册
  if (has_work) {
    pump_->ScheduleWork();
  }
}

bool EventLoop::RunsTasksOnCurrentThread() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bound_ && thread_id_ == std::this_thread::get_id();
}

bool EventLoop::has_exited() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return exited_;
}

std::shared_ptr<TaskRunner> EventLoop::GetTaskRunner() {
  return std::make_shared<EventLoopTaskRunner>(shared_from_this());
}

// ---------------------------------------------------------------------------
// 投递

void EventLoop::PostTask(Closure task, TimePoint target_time) {
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!terminate_posted_) {
      queue_.emplace(Key{target_time, next_sequence_++}, std::move(task));
      if (target_time < next_wake_) {
        next_wake_ = target_time;
        schedule = true;
      }
    }
  }
  if (task) {
    // 已终止: 丢弃。闭包在锁外析构, 防止析构中再次投递造成死锁。
    MOMO_LOG(VERBOSE) << "Task dropped: event loop terminated.";
    return;
  }
  if (schedule) {
    if (target_time <= Clock::now()) {
      pump_->ScheduleWork();
    } else {
      pump_->ScheduleDelayedWork(target_time);
    }
  }
}

void EventLoop::ScheduleDeleteLater(Object* object) {
  MOMO_DCHECK(object != nullptr);
  bool schedule = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!exited_) {
      deferred_deletes_.push_back(object);
      const TimePoint now = Clock::now();
      if (now < next_wake_) {
        next_wake_ = now;
        schedule = true;
      }
      object = nullptr;
    }
  }
  if (object == nullptr) {
    if (schedule) {
      pump_->ScheduleWork();
    }
    return;
  }
  // 循环已结束: 在本线程立即析构;不能在错误线程析构线程归属对象, 宁可泄漏。
  if (RunsTasksOnCurrentThread()) {
    delete object;
  } else {
    MOMO_LOG(ERROR) << "DeleteLater after the owning event loop exited, called off its thread; "
                       "leaking the object.";
  }
}

void EventLoop::Terminate() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (terminate_posted_) {
      return;
    }
    terminate_posted_ = true;
    // 终止点排在"此刻之前已到期的所有任务"之后。
    terminate_key_ = Key{Clock::now(), next_sequence_++};
    next_wake_ = terminate_key_.first;
  }
  pump_->ScheduleWork();
}

// ---------------------------------------------------------------------------
// 运行

void EventLoop::Run() {
  BindToCurrentThread();
  MOMO_CHECK(!running_) << "EventLoop::Run/RunUntilIdle is not reentrant";
  if (has_exited()) {
    return;
  }
  running_ = true;
  pump_running_ = true;
  pump_->Run();
  pump_running_ = false;
  // pump 因外部原因返回(未经 Finish)时, 同样执行结束语义。
  if (!has_exited()) {
    Terminate();
    size_t ignored = 0;
    while (!has_exited()) {
      RunIteration(&ignored);
    }
  }
  running_ = false;
}

size_t EventLoop::RunUntilIdle() {
  BindToCurrentThread();
  MOMO_CHECK(!running_) << "EventLoop::Run/RunUntilIdle is not reentrant";
  running_ = true;
  size_t tasks_run = 0;
  while (RunIteration(&tasks_run)) {
  }
  running_ = false;
  return tasks_run;
}

TimePoint EventLoop::DoWork() {
  size_t ignored = 0;
  RunIteration(&ignored);
  std::lock_guard<std::mutex> lock(mutex_);
  if (exited_) {
    return TimePoint::max();
  }
  next_wake_ = NextWakeLocked();
  return next_wake_;
}

bool EventLoop::RunIteration(size_t* tasks_run) {
  MOMO_DCHECK(RunsTasksOnCurrentThread()) << "EventLoop iterated off its thread";
  std::vector<Closure> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exited_) {
      return false;
    }
    // 本轮重新计算唤醒;期间新的投递会重新向 pump 请求。
    next_wake_ = TimePoint::max();
    const TimePoint now = Clock::now();
    while (!queue_.empty()) {
      auto it = queue_.begin();
      if (it->first.first > now || (terminate_posted_ && terminate_key_ < it->first)) {
        break;
      }
      batch.push_back(std::move(it->second));
      queue_.erase(it);
    }
  }

  for (Closure& task : batch) {
    ++task_depth_;
    task();
    task = nullptr;  // 闭包在任务深度内析构: 其捕获对象的析构也视为"任务中"
    --task_depth_;
  }
  *tasks_run += batch.size();
  bool did_work = !batch.empty();

  // 安全点: 只有最外层(栈上无任务)才析构被标记的对象, 嵌套循环中推迟到外层任务返回。
  if (task_depth_ == 0) {
    if (ProcessDeferredDeletes()) {
      did_work = true;
    }
    bool finish = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      finish = terminate_posted_ && IsDrainedLocked();
    }
    if (finish) {
      Finish();
      did_work = true;
    }
  }
  return did_work;
}

bool EventLoop::ProcessDeferredDeletes() {
  std::vector<Object*> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(deferred_deletes_);
  }
  // 析构过程中新标记的对象进入下一轮(或 Finish 的下一遍)。
  for (Object* object : batch) {
    delete object;
  }
  return !batch.empty();
}

bool EventLoop::IsDrainedLocked() const {
  // 终止点之前投递的任务都已执行(剩余的都排在终止点之后, 将被丢弃)。
  return queue_.empty() || terminate_key_ < queue_.begin()->first;
}

TimePoint EventLoop::NextWakeLocked() const {
  const TimePoint now = Clock::now();
  if (!deferred_deletes_.empty() && task_depth_ == 0) {
    return now;
  }
  if (terminate_posted_ && IsDrainedLocked()) {
    // 只差结束清理(嵌套中则由外层完成)。
    return task_depth_ == 0 ? now : TimePoint::max();
  }
  return queue_.empty() ? TimePoint::max() : queue_.begin()->first.first;
}

void EventLoop::Finish() {
  MOMO_DCHECK(task_depth_ == 0);
  // 1) 丢弃终止点之后的任务: 在本线程析构闭包(其捕获物可能 DeleteLater, 仍会被接收)。
  std::map<Key, Closure> dropped;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dropped.swap(queue_);
  }
  if (!dropped.empty()) {
    MOMO_LOG(VERBOSE) << dropped.size() << " pending task(s) dropped at event loop exit.";
  }
  dropped.clear();

  // 2) 反复清理 DeleteLater 队列, 直到为空;与"置为已结束"在同一临界区判定, 不会漏掉并发标记。
  while (true) {
    ProcessDeferredDeletes();
    std::lock_guard<std::mutex> lock(mutex_);
    if (deferred_deletes_.empty()) {
      queue_.clear();  // 结束期间的投递已被拒收, 这里应为空
      exited_ = true;
      break;
    }
  }

  // 3) 解绑当前线程(允许之后绑定新的循环);RunsTasksOnCurrentThread 仍按线程判定。
  if (g_current_loop == this) {
    g_current_loop = nullptr;
  }
  if (pump_running_) {
    pump_->Quit();
  }
}

}  // namespace momo
