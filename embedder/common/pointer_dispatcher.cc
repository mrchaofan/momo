#include "embedder/common/pointer_dispatcher.h"

#include <utility>

namespace momo::embedder {

using platform::PointerPhase;
using Type = PointerInput::Type;

namespace {

Point ScreenOf(const PointerInput& input) { return {input.data.screen_x, input.data.screen_y}; }

}  // namespace

void PointerDispatcher::Dispatch(const PointerInput& input) {
  last_ = input;
  last_.native = nullptr;  // 只用于之后合成事件的位置与时间
  // 过期: 序列进行中, 系统层却已没有别的按键按住(新的按下, 或无按键的移动)——抬起被嵌套事件循环吞掉了。
  if (sequence_.id != 0 && !input.others_held && (input.type == Type::kDown || input.type == Type::kMove)) {
    FinishSequence(input);
  }
  if (sequence_.id != 0) {
    DispatchInSequence(input);
    return;
  }
  switch (input.type) {
    case Type::kDown:
      BeginSequence(input);
      break;
    case Type::kMove:
      Hover(input);
      break;
    case Type::kLeave:
      if (hover_.window == input.window) SetHover({}, input);
      break;
    case Type::kScroll:
      LockAndSend(scroll_lock_, input, PointerPhase::kScroll);
      break;
    case Type::kGesture:
      LockAndSend(gesture_lock_, input, PointerPhase::kGesture);
      break;
    case Type::kUp:
    case Type::kPressure:
      break;  // 不属于任何序列(按下交给了系统, 如窗口控制按钮)
  }
}

// ---- 按下序列 ----

void PointerDispatcher::BeginSequence(const PointerInput& input) {
  const Target target = HitTarget(input.window, input);
  if (target.window == nullptr) {
    return;  // 不在 momo 窗口的内容区内
  }
  SetHover(target, input);
  sequence_ = Sequence{next_sequence_id_++, target, input.button, std::nullopt};
  Send(target, input, PointerPhase::kDown);
}

void PointerDispatcher::DispatchInSequence(const PointerInput& input) {
  switch (input.type) {
    case Type::kDown:
      sequence_.buttons |= input.button;
      SendToSequence(input, PointerPhase::kMove);
      break;
    case Type::kMove:
      if (sequence_.move_grab && sequence_.target.window != nullptr) {
        // 先移动窗口, 再换算坐标(屏幕坐标不受窗口移动影响)。
        const Point screen = ScreenOf(input);
        sequence_.target.window->MoveTo({screen.x - sequence_.move_grab->x, screen.y - sequence_.move_grab->y});
      }
      SendToSequence(input, PointerPhase::kMove);
      break;
    case Type::kPressure:
      SendToSequence(input, PointerPhase::kGesture);
      break;
    case Type::kUp:
      sequence_.buttons &= ~input.button;
      if (sequence_.buttons != 0) {
        SendToSequence(input, PointerPhase::kMove);
      } else {
        FinishSequence(input);
      }
      break;
    case Type::kScroll:
    case Type::kGesture:
      // 只认接收窗口自己的滚动/手势;别的窗口的丢弃。
      if (input.window == sequence_.target.window) {
        SendToSequence(input, input.type == Type::kScroll ? PointerPhase::kScroll : PointerPhase::kGesture);
      }
      break;
    case Type::kLeave:
      break;  // 序列期间指针离开窗口: 接收方不变
  }
}

void PointerDispatcher::SendToSequence(const PointerInput& input, PointerPhase phase) {
  const Target& target = sequence_.target;
  if (target.window == nullptr) {
    return;  // 接收窗口已关闭
  }
  // 原生事件的坐标属于系统交给它的窗口: 平台视图只收自己窗口的事件。
  if (target.view != 0 && input.window != target.window) {
    return;
  }
  Send(target, input, phase);
}

// 以抬起结束序列, 再按松开处重新判定悬停。input 是最后一个抬起, 或(过期、系统拖动返回)结束时的位置:
// momo 收到抬起;原生视图只收真实的抬起(它的按下状态由系统维护, 补报的事件对它没有意义)。
void PointerDispatcher::FinishSequence(const PointerInput& input) {
  const Target ended = sequence_.target;
  sequence_.buttons = 0;
  if (ended.window != nullptr && ended.view == 0) {
    SendToMomo(ended.window, input, PointerPhase::kUp);
  } else if (ended.window != nullptr && input.type == Type::kUp && input.window == ended.window) {
    ended.window->SendToPlatformView(ended.view, input, PointerPhase::kUp);
  }
  sequence_ = Sequence{};
  const Target now = HitTarget(ended.window, input);
  SetHover(now, input);
  if (now.window != nullptr && now.view == 0 && ended.view != 0) {
    SendToMomo(now.window, input, PointerPhase::kHover);  // 从原生视图的序列回到 momo 内容
  }
}

// ---- 没有序列 ----

void PointerDispatcher::Hover(const PointerInput& input) {
  const Target target = HitTarget(input.window, input);
  SetHover(target, input);
  if (target.window != nullptr) {
    Send(target, input, PointerPhase::kHover);
  }
}

// 滚动/手势: 开始时(或没有阶段的单个事件)命中并锁定, 直到结束。
void PointerDispatcher::LockAndSend(Target& lock, const PointerInput& input, PointerPhase phase) {
  const platform::SignalPhase signal = input.data.signal_phase;
  if (signal == platform::SignalPhase::kStart || signal == platform::SignalPhase::kNone) {
    lock = HitTarget(input.window, input);
  }
  const Target target = lock;
  if (signal == platform::SignalPhase::kEnd || signal == platform::SignalPhase::kNone) {
    lock = {};
  }
  if (target.window != nullptr && (target.view == 0 || input.window == target.window)) {
    Send(target, input, phase);
  }
}

// 悬停目标变化: 原生视图的进入/离开;指针不再在 momo 内容上时 momo 收到离开(kRemove)。
// momo 的接入(kAdd)在下一次交给它事件时补上(SendToMomo)。
void PointerDispatcher::SetHover(Target target, const PointerInput& input) {
  if (target == hover_) {
    return;
  }
  const Target old = hover_;
  hover_ = target;
  if (old.window != nullptr && old.view != 0 && old.window != target.window) {
    old.window->SetHoveredPlatformView(0, input);
  }
  if (momo_ != nullptr && !(target.window == momo_ && target.view == 0)) {
    RemoveFromMomo(input);
  }
  if (target.window != nullptr && (target.view != 0 || (old.window == target.window && old.view != 0))) {
    target.window->SetHoveredPlatformView(target.view, input);
  }
}

PointerDispatcher::Target PointerDispatcher::HitTarget(PointerWindow* window, const PointerInput& input) const {
  if (window == nullptr) {
    return {};
  }
  const std::optional<int64_t> view = window->HitTest(ScreenOf(input));
  return view ? Target{window, *view} : Target{};
}

// ---- 发送 ----

void PointerDispatcher::Send(const Target& target, const PointerInput& input, PointerPhase phase) {
  if (target.view != 0) {
    target.window->SendToPlatformView(target.view, input, phase);
  } else {
    SendToMomo(target.window, input, phase);
  }
}

void PointerDispatcher::SendToMomo(PointerWindow* window, const PointerInput& input, PointerPhase phase) {
  platform::PointerDataPacket packet;
  platform::PointerData data = input.data;
  data.phase = phase;
  data.buttons = sequence_.target.window == window && sequence_.target.view == 0 ? sequence_.buttons : 0;
  data.sequence_id = sequence_.id != 0 && sequence_.target.view == 0 ? sequence_.id : 0;
  if (momo_ != window) {
    if (momo_ != nullptr) RemoveFromMomo(input);
    momo_ = window;
    platform::PointerData add = data;
    add.phase = PointerPhase::kAdd;
    add.buttons = 0;
    add.scroll_delta_x = add.scroll_delta_y = 0;
    add.signal_phase = platform::SignalPhase::kNone;
    add.gesture_kind = platform::PointerGestureKind::kNone;
    packet.push_back(add);
  }
  packet.push_back(data);
  window->SendToMomo(std::move(packet), input);
}

void PointerDispatcher::RemoveFromMomo(const PointerInput& input) {
  PointerWindow* window = std::exchange(momo_, nullptr);
  platform::PointerData data;
  data.timestamp_us = input.data.timestamp_us;
  data.kind = input.data.kind;
  data.device = input.data.device;
  data.screen_x = input.data.screen_x;
  data.screen_y = input.data.screen_y;
  data.phase = PointerPhase::kRemove;
  window->SendToMomo({data}, input);
}

PointerInput PointerDispatcher::Synthesized(Point screen, int64_t timestamp_us) const {
  PointerInput input;
  input.type = Type::kMove;
  input.data.timestamp_us = timestamp_us;
  input.data.screen_x = screen.x;
  input.data.screen_y = screen.y;
  return input;
}

// ---- 窗口能力 ----

bool PointerDispatcher::Capture(PointerWindow* window, int64_t sequence_id) {
  if (sequence_.id == 0 || sequence_.id != sequence_id || sequence_.target.window == nullptr ||
      sequence_.target.view != 0) {
    return false;
  }
  if (sequence_.target.window != window) {
    // 不合成任何事件: 框架的逻辑接收方不变, 同一个指针继续(坐标换成新窗口的)。
    sequence_.target.window = window;
    sequence_.move_grab.reset();
    momo_ = window;
  }
  return true;
}

bool PointerDispatcher::IsMomoSequence(PointerWindow* window, int64_t sequence_id) const {
  return sequence_.id != 0 && sequence_.id == sequence_id && sequence_.target.window == window &&
         sequence_.target.view == 0;
}

void PointerDispatcher::EndSequence(PointerWindow* window, Point screen, int64_t timestamp_us) {
  if (sequence_.id == 0 || sequence_.target.window != window) {
    return;
  }
  PointerInput input = Synthesized(screen, timestamp_us);
  input.window = window;
  FinishSequence(input);
}

bool PointerDispatcher::StartMoveLoop(PointerWindow* window, Point grab) {
  if (sequence_.id == 0 || sequence_.target.window != window || sequence_.target.view != 0) {
    return false;
  }
  sequence_.move_grab = grab;
  const Point screen = ScreenOf(last_);
  window->MoveTo({screen.x - grab.x, screen.y - grab.y});
  return true;
}

void PointerDispatcher::EndMoveLoop(PointerWindow* window) {
  if (sequence_.target.window == window) {
    sequence_.move_grab.reset();
  }
}

void PointerDispatcher::ReleaseCapture(PointerWindow* window) {
  if (sequence_.id == 0 || sequence_.target.window != window) {
    return;
  }
  if (sequence_.target.view == 0 && momo_ == window) {
    PointerInput input = Synthesized(ScreenOf(last_), last_.data.timestamp_us);
    SendToMomo(window, input, PointerPhase::kCancel);
  }
  sequence_.target = {};
  sequence_.move_grab.reset();
}

void PointerDispatcher::RemoveWindow(PointerWindow* window) {
  if (sequence_.target.window == window) {
    sequence_.target = {};
    sequence_.move_grab.reset();
  }
  if (momo_ == window) momo_ = nullptr;
  if (hover_.window == window) hover_ = {};
  if (scroll_lock_.window == window) scroll_lock_ = {};
  if (gesture_lock_.window == window) gesture_lock_ = {};
  if (last_.window == window) last_.window = nullptr;
}

void PointerDispatcher::GeometryChanged(PointerWindow* window) {
  if (sequence_.id != 0 || hover_.window != window) {
    return;  // 序列期间接收方固定;指针不在这个窗口上
  }
  PointerInput input = Synthesized(ScreenOf(last_), last_.data.timestamp_us);
  input.window = window;
  const Target target = HitTarget(window, input);
  if (target == hover_) {
    return;
  }
  SetHover(target, input);
  if (target.window != nullptr && target.view == 0) {
    SendToMomo(window, input, PointerPhase::kHover);  // 指针回到 momo 内容
  }
}

std::optional<int64_t> PointerDispatcher::PointerOwner(const PointerWindow* window) const {
  const Target& owner = sequence_.id != 0 ? sequence_.target : hover_;
  return window != nullptr && owner.window == window ? std::optional<int64_t>(owner.view) : std::nullopt;
}

}  // namespace momo::embedder
