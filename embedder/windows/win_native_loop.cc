#include "embedder/windows/win_native_loop.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

#include "base/logging.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002  // Win10 1803+(旧 SDK 无此宏)
#endif

namespace momo::embedder::windows {

namespace {

constexpr wchar_t kClassName[] = L"MomoNativeLoopMessageWindow";
constexpr UINT kWakeMessage = WM_APP + 1;
constexpr UINT_PTR kWorkTimerId = 1;   // 外部嵌套循环中续跑剩余任务(最短间隔)
constexpr UINT_PTR kDelayTimerId = 2;  // 外部嵌套循环中触发下一个延迟任务
// 自己的泵每轮最多处理的窗口消息数, 之后回到任务(避免消息洪流饿死任务)。
constexpr int kMaxMessagesPerBatch = 64;

UINT ToTimerMillis(TimeDelta delta) {
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         delta + std::chrono::microseconds(999))  // 向上取整, 避免早到
                         .count();
  return static_cast<UINT>(std::clamp<int64_t>(ms, USER_TIMER_MINIMUM, USER_TIMER_MAXIMUM));
}

}  // namespace

WinNativeLoop::WinNativeLoop() : thread_id_(::GetCurrentThreadId()) {
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &WinNativeLoop::WndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    MOMO_LOG(ERROR) << "RegisterClassExW(message window) failed: " << ::GetLastError();
  }
  hwnd_ = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance,
                            this);
  MOMO_CHECK(hwnd_ != nullptr) << "CreateWindowExW(HWND_MESSAGE) failed: " << ::GetLastError();

  wait_timer_ = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
  if (wait_timer_ == nullptr) {
    // Win10 1803 之前不支持高精度定时器。
    wait_timer_ = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  if (wait_timer_ == nullptr) {
    MOMO_LOG(WARNING) << "CreateWaitableTimerExW failed (" << ::GetLastError()
                      << "); delayed tasks fall back to millisecond wait timeouts.";
  }
}

WinNativeLoop::~WinNativeLoop() {
  MOMO_DCHECK(RunsTasksOnCurrentThread()) << "WinNativeLoop destroyed off the main thread";
  if (hwnd_ != nullptr) {
    ::KillTimer(hwnd_, kWorkTimerId);
    ::KillTimer(hwnd_, kDelayTimerId);
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (wait_timer_ != nullptr) {
    ::CloseHandle(wait_timer_);
    wait_timer_ = nullptr;
  }
  // 队列中剩余的闭包随成员在主线程析构。
}

bool WinNativeLoop::RunsTasksOnCurrentThread() const {
  return ::GetCurrentThreadId() == thread_id_;
}

void WinNativeLoop::PostTask(Closure task) {
  const bool on_main = RunsTasksOnCurrentThread();
  // 主线程在执行任务期间自我投递: 不 PostMessage(posted 消息优先级高于输入, 连续自投递会饿死输入),
  // 由批次结束后的逻辑(自己的泵直接续跑 / 外部嵌套循环用 SetTimer)接上。
  const bool defer_to_timer = on_main && do_tasks_depth_ > 0;
  bool post = false;
  {
    MutexLock lock(mutex_);
    immediate_.push_back(std::move(task));
    if (!defer_to_timer && !wake_pending_) {
      wake_pending_ = true;
      post = true;
    }
  }
  if (defer_to_timer) {
    // 若此刻处于任务内部启动的外部嵌套循环(如任务里弹 MessageBox), 只有 WM_TIMER 能把我们唤醒。
    ArmWorkTimer();
  }
  if (post) {
    PostWakeMessage();
  }
}

void WinNativeLoop::PostDelayedTask(Closure task, TimeDelta delay) {
  if (delay <= TimeDelta::zero()) {
    PostTask(std::move(task));
    return;
  }
  const bool on_main = RunsTasksOnCurrentThread();
  const TimePoint when = Clock::now() + delay;
  bool post = false;
  bool earliest = false;
  {
    MutexLock lock(mutex_);
    earliest = delayed_.empty() || when < delayed_.begin()->first;
    delayed_.emplace(when, std::move(task));
    // 他线程投递了新的最早任务: 唤醒主线程重新计算定时器。
    if (earliest && !on_main && !wake_pending_) {
      wake_pending_ = true;
      post = true;
    }
  }
  if (on_main && earliest) {
    UpdateForeignTimer();  // 自己的泵在等待前会重新计算;这里只需更新外部嵌套循环用的 SetTimer
  }
  if (post) {
    PostWakeMessage();
  }
}

void WinNativeLoop::PostWakeMessage() {
  if (!::PostMessageW(hwnd_, kWakeMessage, 0, 0)) {
    // 消息队列满(默认 10000 条)等极端情况: 撤销在途标记, 下一次投递再试。
    MOMO_LOG(ERROR) << "PostMessageW(wake) failed: " << ::GetLastError();
    MutexLock lock(mutex_);
    wake_pending_ = false;
  }
}

void WinNativeLoop::Run() {
  MOMO_DCHECK(RunsTasksOnCurrentThread()) << "WinNativeLoop::Run off the main thread";
  if (quit_pending_) {
    quit_pending_ = false;
    return;
  }
  RunState state;
  run_stack_.push_back(&state);
  while (!state.quit) {
    DoTasks();
    if (state.quit) {
      break;
    }
    const bool more_messages = PumpMessages(state);
    if (state.quit) {
      break;
    }
    if (!more_messages) {
      WaitForWork();
    }
  }
  run_stack_.pop_back();
}

void WinNativeLoop::Quit() {
  if (!RunsTasksOnCurrentThread()) {
    // 到主线程再决定"最内层"是谁。
    PostTask([this] { Quit(); });
    return;
  }
  if (run_stack_.empty()) {
    quit_pending_ = true;
  } else {
    run_stack_.back()->quit = true;
  }
}

void WinNativeLoop::ArmWorkTimer() {
  if (!work_timer_armed_) {
    ::SetTimer(hwnd_, kWorkTimerId, USER_TIMER_MINIMUM, nullptr);
    work_timer_armed_ = true;
  }
}

void WinNativeLoop::DisarmWorkTimer() {
  if (work_timer_armed_) {
    ::KillTimer(hwnd_, kWorkTimerId);
    work_timer_armed_ = false;
  }
}

void WinNativeLoop::DoTasks() {
  DisarmWorkTimer();  // 本批次即"续跑";自己的泵里不需要它周期性唤醒
  ++do_tasks_depth_;
  size_t budget = 0;
  {
    MutexLock lock(mutex_);
    wake_pending_ = false;
    budget = immediate_.size();  // 只执行此刻已在队列中的任务: 自我重投的任务留到下一轮
  }
  for (; budget > 0 && !QuitRequested(); --budget) {
    Closure task;
    {
      MutexLock lock(mutex_);
      if (immediate_.empty()) {
        break;  // 嵌套 Run 已经执行掉了
      }
      task = std::move(immediate_.front());
      immediate_.pop_front();
    }
    task();
  }
  const TimePoint now = Clock::now();
  while (!QuitRequested()) {
    Closure task;
    {
      MutexLock lock(mutex_);
      if (delayed_.empty() || delayed_.begin()->first > now) {
        break;
      }
      task = std::move(delayed_.begin()->second);
      delayed_.erase(delayed_.begin());
    }
    task();
  }
  --do_tasks_depth_;
  UpdateForeignTimer();
}

void WinNativeLoop::DoTasksFromForeignLoop() {
  DoTasks();
  bool more = false;
  {
    MutexLock lock(mutex_);
    more = !immediate_.empty() || (!delayed_.empty() && delayed_.begin()->first <= Clock::now());
  }
  if (more) {
    // WM_TIMER 优先级最低: 外部循环先处理输入与绘制, 再续跑剩余任务。
    ArmWorkTimer();
  }
}

void WinNativeLoop::UpdateForeignTimer() {
  TimePoint next = TimePoint::max();
  {
    MutexLock lock(mutex_);
    if (!delayed_.empty()) {
      next = delayed_.begin()->first;
    }
  }
  if (next == TimePoint::max()) {
    if (foreign_timer_armed_) {
      ::KillTimer(hwnd_, kDelayTimerId);
      foreign_timer_armed_ = false;
    }
    return;
  }
  // SetTimer 同 id 会替换旧定时器;WM_TIMER 周期性触发, 每次 DoTasks 后在这里重设。
  ::SetTimer(hwnd_, kDelayTimerId, ToTimerMillis(next - Clock::now()), nullptr);
  foreign_timer_armed_ = true;
}

bool WinNativeLoop::PumpMessages(RunState& state) {
  MSG msg;
  for (int i = 0; i < kMaxMessagesPerBatch; ++i) {
    if (!::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      return false;
    }
    if (msg.message == WM_QUIT) {
      MOMO_LOG(INFO) << "WM_QUIT received; quitting the innermost native loop";
      state.quit = true;
      return false;
    }
    if (msg.hwnd == hwnd_ && (msg.message == kWakeMessage || msg.message == WM_TIMER)) {
      continue;  // 只是唤醒信号: 任务由本泵在下一轮 DoTasks 直接执行
    }
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
    if (state.quit) {
      return false;
    }
  }
  return true;
}

void WinNativeLoop::WaitForWork() {
  TimePoint next = TimePoint::max();
  {
    MutexLock lock(mutex_);
    if (!immediate_.empty()) {
      return;
    }
    if (!delayed_.empty()) {
      next = delayed_.begin()->first;
    }
  }
  DWORD timeout = INFINITE;
  DWORD handle_count = 0;
  if (next != TimePoint::max()) {
    const TimeDelta delta = next - Clock::now();
    if (delta <= TimeDelta::zero()) {
      return;
    }
    if (wait_timer_ != nullptr) {
      // 相对时间: 负值, 单位 100ns。
      const int64_t ticks =
          std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count() / 100);
      LARGE_INTEGER due;
      due.QuadPart = -ticks;
      if (::SetWaitableTimer(wait_timer_, &due, 0, nullptr, nullptr, FALSE)) {
        handle_count = 1;
      }
    }
    if (handle_count == 0) {
      timeout = ToTimerMillis(delta);
    }
  }
  // 消息(含他线程的 kWakeMessage)、输入或定时器到期即返回。
  // MWMO_INPUTAVAILABLE: 队列中已有但尚未取出的输入也视为可用。
  const DWORD result = ::MsgWaitForMultipleObjectsEx(
      handle_count, handle_count != 0 ? &wait_timer_ : nullptr, timeout, QS_ALLINPUT,
      MWMO_INPUTAVAILABLE);
  if (result == WAIT_FAILED) {
    MOMO_LOG(ERROR) << "MsgWaitForMultipleObjectsEx failed: " << ::GetLastError();
  }
}

LRESULT CALLBACK WinNativeLoop::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* self = reinterpret_cast<WinNativeLoop*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self != nullptr) {
    // 能到达这里说明消息由外部(原生)嵌套循环分发: 自己的泵不分发这两类消息。
    if (message == kWakeMessage) {
      self->DoTasksFromForeignLoop();
      return 0;
    }
    if (message == WM_TIMER && (wparam == kWorkTimerId || wparam == kDelayTimerId)) {
      self->DoTasksFromForeignLoop();
      return 0;
    }
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace momo::embedder::windows
