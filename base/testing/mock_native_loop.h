#pragma once

// 测试用 NativeLoop: QueueNativeLoop 加上 Run / Quit 的调用计数。base 与上层测试(embedder 的 mock 平台)共用。

#include "base/queue_native_loop.h"

namespace momo::testing {

class MockNativeLoop final : public QueueNativeLoop {
 public:
  void Run() override {
    run_count_++;
    QueueNativeLoop::Run();
  }
  void Quit() override {
    quit_count_++;
    QueueNativeLoop::Quit();
  }

  int run_count() const { return run_count_; }
  int quit_count() const { return quit_count_; }

 private:
  int run_count_ = 0;
  int quit_count_ = 0;
};

}  // namespace momo::testing
