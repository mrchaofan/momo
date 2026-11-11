#pragma once

// NodeBindings: libuv 事件循环与 momo ui 线程的整合(docs/04 §1;对照 Electron shell/common/node_bindings)。
// 只能被 engine/runtime 的实现文件包含。
//
//   ui 线程(V8 + momo EventLoop)                  poll 线程 "momo.uv.poll"
//   ─────────────────────────────                  ─────────────────────────────────────────
//   StartPolling() → UvRunOnce()                    loop:
//   UvRunOnce():                                       uv_sem_wait(&embed_sem_)   ← 等 ui 跑完一轮
//     HandleScope + Context::Scope                     PollEvents(): 等 uv_backend_fd 可读,
//     uv_run(loop, UV_RUN_NOWAIT)                        超时 uv_backend_timeout(只等待, 不执行回调)
//     uv_sem_post(&embed_sem_) ───────────────────►    WakeupMainThread(): 投递 UvRunOnce 到 ui
//
// uv 回调(Node 的定时器、IO、V8 前台任务…)全部在 ui 线程执行;poll 线程只等内核事件。uv_sem 保证两者
// 严格交替: poll 线程等待时 ui 不在跑 uv, ui 跑 uv 时 poll 线程停在信号量上。一个 uv_async 空句柄保持
// 循环存活(否则 uv_backend_timeout 返回 0, poll 线程空转), 也用于唤醒 poll 线程。
//
// 与 Electron 的差异: Electron 给 libuv 打了 uv_loop_interrupt 补丁——ui 线程上的 JS 新增定时器/IO 时
// 打断 poll 线程, 使其按新的超时重新等待。momo 使用上游 libuv, 改为在每次从 momo 进入 JS 之后
// (JsRuntimeImpl::DrainMicrotasks)调用 WakeupEmbedThread(), 效果相同(uv_async_send 自带合并)。
//
// 线程约定: 除 WakeupEmbedThread 外只在 ui 线程调用;析构须在 ui 线程, 且早于 uv 循环关闭。

#include <atomic>
#include <memory>

#include "base/macros.h"
#include "base/task_runner.h"
#include "uv.h"

namespace node {
class Environment;
}

namespace momo::engine::runtime {

class NodeBindings {
 public:
  // env 与 loop 由调用方持有, 须比本对象活得久;runner 为 ui 线程(isolate 归属线程)的 runner。
  NodeBindings(node::Environment* env, uv_loop_t* loop, std::shared_ptr<TaskRunner> runner);
  ~NodeBindings();  // 隐式 Stop()

  // 创建保活句柄、信号量与 poll 线程(线程先停在信号量上)。
  void PrepareEmbedThread();
  // 跑一轮 uv(让 uv__io_poll 注册全部事件), 然后放行 poll 线程。
  void StartPolling();
  // 结束 poll 线程并关闭保活句柄(句柄在下一次 uv 循环运行时完成关闭)。幂等。
  void Stop();

  // 让 poll 线程立即醒来、按当前的 uv 状态重新等待(ui 线程上的 JS 改动了定时器/IO 之后)。任意线程。
  void WakeupEmbedThread();

  // 保活句柄是否计入循环存活(uv_loop_alive)。入口模块泵送期间置为 false: 循环只剩它时视为没有可做的事。
  void SetKeepAlive(bool keep_alive);

  uv_loop_t* loop() const { return loop_; }

 private:
  static void EmbedThreadRunner(void* arg);
  // 等待 uv 后端(macOS: kqueue fd)可读或超时;平台相关实现(node_bindings_mac.cc)。
  void PollEvents();
  void WakeupMainThread();
  void UvRunOnce();

  node::Environment* const env_;
  uv_loop_t* const loop_;
  const std::shared_ptr<TaskRunner> runner_;
  // 投递到 ui 的 UvRunOnce 持有它的弱引用: 本对象析构后, 已投递的任务作废。
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);

  uv_async_t* dummy_ = nullptr;  // 保活 + 唤醒;关闭回调中释放
  uv_sem_t embed_sem_;
  uv_thread_t embed_thread_;
  bool prepared_ = false;
  bool polling_ = false;
  std::atomic<bool> embed_closed_{false};

  MOMO_DISALLOW_COPY_AND_ASSIGN(NodeBindings);
};

}  // namespace momo::engine::runtime
