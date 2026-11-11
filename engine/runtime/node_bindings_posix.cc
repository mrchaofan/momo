// NodeBindings::PollEvents 的 POSIX(macOS kqueue / Linux epoll)实现: poll(2) 等待 uv 后端 fd 可读,
// 超时取 uv_backend_timeout(-1 = 无限)。同 Electron shell/common/node_bindings_mac.cc。

#include <errno.h>
#include <poll.h>

#include "engine/runtime/node_bindings.h"

namespace momo::engine::runtime {

void NodeBindings::PollEvents() {
  // 与 ui 线程上的 JS 并发读取是良性的: JS 改动定时器/IO 后 WakeupEmbedThread 会让这里按新状态重新等待。
  const int timeout_ms = uv_backend_timeout(loop_);
  struct pollfd pfd;
  pfd.fd = uv_backend_fd(loop_);
  pfd.events = POLLIN;
  pfd.revents = 0;
  int r;
  do {
    r = poll(&pfd, 1, timeout_ms);
  } while (r == -1 && errno == EINTR);
}

}  // namespace momo::engine::runtime
