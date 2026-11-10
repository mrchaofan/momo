#pragma once

// Mutex / MutexLock: 上层(platform/engine/embedder)使用的互斥锁(docs/10 §1: 上层不直接用 std::mutex)。
// 只用于"小临界区保护共享表"这类场景;跨线程协作优先消息传递 + 不可变数据(docs/08 §3)。
// header-only, 属于 momo_base。

#include <mutex>

#include "base/macros.h"

namespace momo {

class Mutex {
 public:
  Mutex() = default;
  void Lock() { mutex_.lock(); }
  void Unlock() { mutex_.unlock(); }

 private:
  std::mutex mutex_;
  MOMO_DISALLOW_COPY_AND_ASSIGN(Mutex);
};

// RAII 作用域锁。
class MutexLock {
 public:
  explicit MutexLock(Mutex& mutex) : mutex_(mutex) { mutex_.Lock(); }
  ~MutexLock() { mutex_.Unlock(); }

 private:
  Mutex& mutex_;
  MOMO_DISALLOW_COPY_AND_ASSIGN(MutexLock);
};

}  // namespace momo
