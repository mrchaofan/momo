#pragma once

// EventLoop: 所有线程(platform/ui/raster/io)统一的事件循环(docs/08 §2、§5)。
// 负责任务调度与 Object 的延迟析构;等待/唤醒与底层原生循环交给 MessagePump。
//
// 一轮迭代(DoWork):
//   1. 取出"此刻已到期"任务的快照, 按 (到期时间, 投递序号) 顺序执行。执行期间新投递的任务进入下一轮。
//   2. 若栈上已无任务(非嵌套), 析构所有被 DeleteLater 标记的对象 —— 安全点。
//      嵌套循环(某个任务内部跑了模态框等原生循环, pump 重入 DoWork)只执行任务, 不析构,
//      直到最外层任务返回。
//   3. 返回下一次需要唤醒的时刻, 由 pump 等待。
//
// 结束语义(Terminate, 任意线程):
//   - Terminate 之前已到期的任务全部执行;未到期的延迟任务与之后投递的任务被丢弃(在循环线程析构)。
//   - 循环结束时反复清理 DeleteLater 队列直到为空: 被标记的对象一定在本线程析构, 绝不丢失。
//   - 结束后调用 DeleteLater: 在循环线程上立即析构;其他线程上调用则记 ERROR 并泄漏(不在错误线程析构)。
//
// 线程绑定: 每个循环只绑定一个线程, 每个线程至多绑定一个循环(EventLoop::Current())。
// Run/RunUntilIdle 不可嵌套调用。

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "base/closure.h"
#include "base/macros.h"
#include "base/message_pump.h"
#include "base/task_runner.h"
#include "base/time.h"

namespace momo {

class Object;

class EventLoop final : public MessagePump::Delegate, public std::enable_shared_from_this<EventLoop> {
 public:
  // pump 为空时用 DefaultMessagePump。
  static std::shared_ptr<EventLoop> Create(std::unique_ptr<MessagePump> pump = nullptr);
  ~EventLoop();

  // 当前线程绑定的循环;已结束或未绑定时为 nullptr。
  static EventLoop* Current();

  // 绑定到当前线程(Run/RunUntilIdle 会自动绑定)。提前绑定可让 RunsTasksOnCurrentThread 与
  // Object 的默认归属在开始运行前就生效。
  void BindToCurrentThread();

  // 在当前线程运行 pump, 直到 Terminate 后排空;返回前完成结束清理。已结束则立即返回。
  void Run();
  // 不经 pump, 在当前线程连续迭代, 直到没有已到期任务与待析构对象(不等待延迟任务)。
  // 若已 Terminate 且排空, 会完成结束清理。返回执行的任务数。用于测试与宿主收尾。
  size_t RunUntilIdle();
  // 请求结束(任意线程, 幂等)。
  void Terminate();

  // 在循环线程上的原生同步等待中(如窗口缩放时主线程等待新尺寸的帧)执行一轮已到期任务, 不经 pump。
  // 视为嵌套循环: 不析构 DeleteLater 对象(推迟到外层安全点), 不做结束清理。返回执行的任务数。仅循环线程,
  // 且须在 Run 期间(pump 正在服务本循环)。
  size_t RunDueTasksNested();

  void PostTask(Closure task, TimePoint target_time);
  // 由 TaskRunner::ScheduleDeleteLater 调用(经 Object::DeleteLater)。任意线程。
  void ScheduleDeleteLater(Object* object);

  bool RunsTasksOnCurrentThread() const;
  bool has_exited() const;
  // 当前处于第几层任务中(0 = 栈上无任务)。仅循环线程。
  int task_depth() const { return task_depth_; }

  // 投递到本循环的 TaskRunner(持有循环的共享引用)。
  std::shared_ptr<TaskRunner> GetTaskRunner();

 private:
  explicit EventLoop(std::unique_ptr<MessagePump> pump);

  // MessagePump::Delegate
  TimePoint DoWork() override;

  // 执行一轮;返回是否做了事(执行了任务或析构了对象)。
  bool RunIteration(size_t* tasks_run);
  bool ProcessDeferredDeletes();  // 返回是否析构了对象
  bool IsDrainedLocked() const;
  TimePoint NextWakeLocked() const;
  void Finish();

  using Key = std::pair<TimePoint, uint64_t>;  // (到期时间, 投递序号) 保证同时刻 FIFO

  std::unique_ptr<MessagePump> pump_;

  mutable std::mutex mutex_;
  std::map<Key, Closure> queue_;
  std::vector<Object*> deferred_deletes_;
  uint64_t next_sequence_ = 0;
  bool terminate_posted_ = false;
  Key terminate_key_{};
  bool exited_ = false;
  TimePoint next_wake_ = TimePoint::max();  // 已向 pump 请求的最早唤醒时刻
  std::thread::id thread_id_;
  bool bound_ = false;

  // 仅循环线程。
  bool running_ = false;       // Run 或 RunUntilIdle 进行中
  bool pump_running_ = false;  // pump_->Run 进行中
  int task_depth_ = 0;

  MOMO_DISALLOW_COPY_AND_ASSIGN(EventLoop);
};

}  // namespace momo
