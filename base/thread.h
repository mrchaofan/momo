#pragma once

// Thread: 拥有一个运行 EventLoop 的具名 OS 线程(如 "momo.ui")。
// 线程约定: 在创建线程上构造/Join/析构。析构隐式 Join。
//
// 栈大小可配置(默认 8 MiB): macOS 次线程默认只有 512 KiB, 而 V8 默认按 ~1 MiB 的栈上限做检查,
// JS 运行在引擎自建的 ui 线程上, 栈不足会直接越界崩溃。

#include <cstddef>
#include <memory>
#include <string>

#include "base/event_loop.h"
#include "base/macros.h"
#include "base/task_runner.h"

namespace momo {

class Thread {
 public:
  struct Options {
    size_t stack_size = 8u << 20;  // 0 = 系统默认
  };

  // 启动线程并等待其消息循环就绪, 返回时 GetTaskRunner() 立即可用。
  explicit Thread(std::string name) : Thread(std::move(name), Options{}) {}
  Thread(std::string name, Options options);
  ~Thread();

  const std::string& name() const { return name_; }
  const std::shared_ptr<TaskRunner>& GetTaskRunner() const { return task_runner_; }

  // 终止事件循环(排空已到期任务、清理 DeleteLater 对象)并 join。幂等。
  void Join();
  bool joined() const { return joined_; }

  // 设置当前线程名(调试器/profiler 可见)。
  static void SetCurrentThreadName(const std::string& name);

 private:
  struct Native;  // 平台线程句柄(pthread_t / HANDLE)

  void ThreadMain();

  std::string name_;
  std::shared_ptr<EventLoop> loop_;
  std::shared_ptr<TaskRunner> task_runner_;
  std::unique_ptr<Native> native_;
  bool joined_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Thread);
};

}  // namespace momo
