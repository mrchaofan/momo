#pragma once

// WinNativeLoop: base::NativeLoop 的 Win32 实现(docs/03 §1, docs/12 §3)。仅被 embedder/windows 的 .cc 包含。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
// 机制:
//   - 构造线程即 OS 主线程(记录线程 id, 因此 RunsTasksOnCurrentThread 在 Run 之前也正确)。
//   - 任务队列在本对象内(Mutex 保护): 立即任务 FIFO + 延迟任务按 (到期时间, 投递顺序) 排序。
//   - 唤醒: 向隐藏的消息窗口(HWND_MESSAGE)PostMessage(kWakeMessage);同一时刻至多一个在途(合并)。
//   - Run(): 自己的消息泵 —— 先执行一批任务, 再处理一批窗口消息(含输入), 然后
//     MsgWaitForMultipleObjectsEx 等待"消息到达 / 高精度可等待定时器(下一个延迟任务到期)"。
//     在自己的泵里, 发往消息窗口的 kWakeMessage / WM_TIMER 不分发(只是唤醒信号), 任务由泵直接执行;
//     这样即使任务不断自我重投, 输入消息也不会被"优先级更高的 posted 消息"饿死。
//   - 外部(原生)嵌套循环: 窗口拖动/缩放的模态循环、MessageBox、菜单等由系统自己 GetMessage/
//     DispatchMessage, 我们的泵不运行。此时 kWakeMessage 被分发到消息窗口的 WndProc 并在其中执行任务;
//     延迟任务靠 SetTimer(WM_TIMER, 精度 ~10–16ms)。主线程在执行任务期间自我投递的任务不 PostMessage,
//     而是在批次结束后用最短 SetTimer 续上(WM_TIMER 优先级最低, 不会饿死输入)。
//   - PostDelayedTask 精度: 自己的泵内用 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 定时器(Win10 1803+,
//     不可用时退回普通可等待定时器, 精度受系统时钟中断 ~15.6ms 限制);外部嵌套循环内为 SetTimer 精度。
//   - Run 可嵌套;Quit 只结束最内层 Run(在主线程上置位最内层的退出标志;他线程调用则投递到主线程再执行)。
//     未运行时调用 Quit: 下一次 Run 立即返回(与 testing::MockNativeLoop 一致)。WM_QUIT 同样只结束最内层。
// 线程约定: PostTask/PostDelayedTask/RunsTasksOnCurrentThread/Quit 任意线程;其余仅主线程。
// 生命周期: 构造/析构在主线程;析构后不得再投递(由 NativeMessagePump 的契约保证: native loop 活得比 pump 久)。

#include <deque>
#include <map>
#include <vector>

#include "base/lock.h"
#include "base/macros.h"
#include "base/native_loop.h"
#include "embedder/windows/win_util.h"

namespace momo::embedder::windows {

class WinNativeLoop final : public NativeLoop {
 public:
  WinNativeLoop();
  ~WinNativeLoop() override;

  void PostTask(Closure task) override;
  void PostDelayedTask(Closure task, TimeDelta delay) override;
  bool RunsTasksOnCurrentThread() const override;
  void Run() override;
  void Quit() override;

  // 隐藏消息窗口(供剪贴板 owner 等使用)。仅主线程使用。
  HWND message_window() const { return hwnd_; }

 private:
  struct RunState {
    bool quit = false;
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  // 向消息窗口投递 kWakeMessage(任意线程;调用方已在锁内置位 wake_pending_)。
  void PostWakeMessage();
  // 主线程: 启动/停止"续跑剩余任务"的最短 SetTimer。
  void ArmWorkTimer();
  void DisarmWorkTimer();
  // 主线程: 执行一批任务(此刻已在队列中的立即任务 + 已到期的延迟任务)。
  void DoTasks();
  // 主线程: 在外部嵌套循环中被唤醒(kWakeMessage / WM_TIMER)。
  void DoTasksFromForeignLoop();
  // 主线程: 按下一个延迟任务重设 SetTimer(外部嵌套循环用)。
  void UpdateForeignTimer();
  // 主线程(自己的泵): 处理一批窗口消息;收到 WM_QUIT 时置位 state.quit。返回是否可能还有消息。
  bool PumpMessages(RunState& state);
  // 主线程(自己的泵): 无事可做时阻塞等待消息或定时器。
  void WaitForWork();
  bool QuitRequested() const { return !run_stack_.empty() && run_stack_.back()->quit; }

  const DWORD thread_id_;
  HWND hwnd_ = nullptr;
  HANDLE wait_timer_ = nullptr;  // 可等待定时器(自己的泵等待下一个延迟任务)

  Mutex mutex_;
  std::deque<Closure> immediate_;               // mutex_
  std::multimap<TimePoint, Closure> delayed_;   // mutex_;等键按插入顺序(FIFO)
  bool wake_pending_ = false;                   // mutex_: 已有在途 kWakeMessage

  // 以下仅主线程。
  std::vector<RunState*> run_stack_;
  bool quit_pending_ = false;    // 未运行时收到 Quit
  int do_tasks_depth_ = 0;       // >0: 正在执行任务(主线程自投递不 PostMessage)
  bool foreign_timer_armed_ = false;  // kDelayTimerId
  bool work_timer_armed_ = false;     // kWorkTimerId

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinNativeLoop);
};

}  // namespace momo::embedder::windows
