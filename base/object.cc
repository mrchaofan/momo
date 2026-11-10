#include "base/object.h"

#include <utility>

#include "base/event_loop.h"

namespace momo {

namespace {

std::shared_ptr<TaskRunner> CurrentLoopRunner() {
  EventLoop* loop = EventLoop::Current();
  MOMO_CHECK(loop != nullptr)
      << "momo::Object created on a thread without an EventLoop; pass an explicit affinity";
  return loop->GetTaskRunner();
}

}  // namespace

Object::Object() : Object(CurrentLoopRunner()) {}

Object::Object(std::shared_ptr<TaskRunner> affinity)
    : task_runner_(std::move(affinity)), liveness_(std::make_shared<Liveness>()) {
  MOMO_CHECK(task_runner_ != nullptr);
}

Object::~Object() {
  MOMO_DCHECK(IsOnAffinityThread()) << "momo::Object destroyed off its affinity thread";
  liveness_->alive = false;
}

void Object::InvalidateWeakPtrs() {
  MOMO_DCHECK(IsOnAffinityThread());
  liveness_->alive = false;
  liveness_ = std::make_shared<Liveness>();
}

void Object::DeleteLater() {
  if (delete_scheduled_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  task_runner_->ScheduleDeleteLater(this);
}

}  // namespace momo
