// NodeBindings::PollEvents 的 Windows 实现: 在 libuv 的完成端口上等待一个完成包, 超时取 uv_backend_timeout
// (-1 = INFINITE);拿到的完成包原样放回端口, 由 ui 线程上的 uv_run 处理(只等待, 不消费)。
// 同 Electron shell/common/node_bindings_win.cc。

#include <windows.h>

#include "engine/runtime/node_bindings.h"

namespace momo::engine::runtime {

void NodeBindings::PollEvents() {
  // 与 ui 线程上的 JS 并发读取是良性的: JS 改动定时器/IO 后 WakeupEmbedThread 会让这里按新状态重新等待。
  const int timeout_ms = uv_backend_timeout(loop_);
  DWORD bytes = 0;
  ULONG_PTR key = 0;
  OVERLAPPED* overlapped = nullptr;
  ::GetQueuedCompletionStatus(loop_->iocp, &bytes, &key, &overlapped,
                              timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  // 把事件交还给 libuv(它在 ui 线程的 uv_run 里取出并分发)。
  if (overlapped != nullptr) {
    ::PostQueuedCompletionStatus(loop_->iocp, bytes, key, overlapped);
  }
}

}  // namespace momo::engine::runtime
