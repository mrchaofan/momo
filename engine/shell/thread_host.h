#pragma once

// ThreadHost: 创建并拥有引擎的 ui/raster/io 三个具名线程(momo.ui 等), 产出 TaskRunners。
// platform runner 不在此创建: 它是 OS 主线程的消息循环, 由宿主(embedder / 测试)提供。
//
// 所有权留在宿主: 宿主持有 ThreadHost, 在 Shell 关闭且窗口销毁之后调用 Join()(docs/01 §4)。
// Join 顺序固定为 ui → raster → io: ui/raster 的收尾仍可能向 io 投递。
// 线程约定: 在宿主线程(通常是 platform 线程)构造、Join、析构。析构隐式 Join。

#include <memory>
#include <string>

#include "base/macros.h"
#include "base/task_runner.h"
#include "base/thread.h"
#include "engine/common/task_runners.h"

namespace momo::engine {

class ThreadHost {
 public:
  // label 用作线程名前缀: "<label>.ui" / "<label>.raster" / "<label>.io"。
  explicit ThreadHost(std::string label = "momo");
  ~ThreadHost();

  // 组装四个 runner。platform 由调用方提供(不可为空)。
  TaskRunners MakeTaskRunners(std::shared_ptr<TaskRunner> platform) const;

  // 按 ui → raster → io 终止并 join(各自排空已到期任务)。幂等。
  void Join();
  bool joined() const { return joined_; }

  const std::string& label() const { return label_; }

 private:
  std::string label_;
  Thread ui_;
  Thread raster_;
  Thread io_;
  bool joined_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(ThreadHost);
};

}  // namespace momo::engine
