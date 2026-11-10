#include "base/native_message_pump.h"

#include <utility>

namespace momo {

NativeMessagePump::NativeMessagePump(NativeLoop& native)
    : native_(native), state_(std::make_shared<State>()) {}

NativeMessagePump::~NativeMessagePump() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->delegate = nullptr;
}

void NativeMessagePump::Attach(Delegate* delegate) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->delegate = delegate;
}

void NativeMessagePump::Run() { native_.Run(); }

void NativeMessagePump::Quit() { native_.Quit(); }

void NativeMessagePump::ScheduleWork() {
  native_.PostTask([state = state_, &native = native_] { RunDoWork(state, native); });
}

void NativeMessagePump::ScheduleDelayedWork(TimePoint when) {
  ScheduleDelayed(state_, native_, when);
}

void NativeMessagePump::ScheduleDelayed(const std::shared_ptr<State>& state,
                                         NativeLoop& native, TimePoint when) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (when >= state->pending_delayed) {
      return;  // 已有更早的在途唤醒, 它触发后 DoWork 会返回新的下一次时刻
    }
    state->pending_delayed = when;
  }
  const TimeDelta delay = when - Clock::now();
  native.PostDelayedTask(
      [state, &native, when] {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (state->pending_delayed == when) {
            state->pending_delayed = TimePoint::max();
          }
        }
        RunDoWork(state, native);
      },
      delay > TimeDelta::zero() ? delay : TimeDelta::zero());
}

void NativeMessagePump::RunDoWork(const std::shared_ptr<State>& state,
                                   NativeLoop& native) {
  Delegate* delegate = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    delegate = state->delegate;
  }
  if (delegate == nullptr) {
    return;  // pump 已析构
  }
  const TimePoint next = delegate->DoWork();
  if (next == TimePoint::max()) {
    return;
  }
  if (next <= Clock::now()) {
    native.PostTask([state, &native] { RunDoWork(state, native); });
  } else {
    ScheduleDelayed(state, native, next);
  }
}

}  // namespace momo
