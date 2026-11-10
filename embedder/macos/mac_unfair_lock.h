#pragma once

// os_unfair_lock 的 RAII 小封装(平台实现与系统回调线程之间的短临界区用;
// docs/10 §1: 上层不直接使用 std::mutex)。只在 .mm/.cc 中使用。

#include <os/lock.h>

#include "base/macros.h"

namespace momo::embedder::macos {

class UnfairLock {
 public:
  UnfairLock() = default;
  void Lock() { os_unfair_lock_lock(&lock_); }
  void Unlock() { os_unfair_lock_unlock(&lock_); }

 private:
  os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
  MOMO_DISALLOW_COPY_AND_ASSIGN(UnfairLock);
};

class UnfairLockScope {
 public:
  explicit UnfairLockScope(UnfairLock& lock) : lock_(lock) { lock_.Lock(); }
  ~UnfairLockScope() { lock_.Unlock(); }

 private:
  UnfairLock& lock_;
  MOMO_DISALLOW_COPY_AND_ASSIGN(UnfairLockScope);
};

}  // namespace momo::embedder::macos
