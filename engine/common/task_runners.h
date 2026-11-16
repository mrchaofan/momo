#pragma once

// TaskRunners: 四个 TaskRunner 的集合(ARCHITECTURE.md §1.3)。值类型, 可随意拷贝。

#include <memory>

#include "base/task_runner.h"

namespace momo::engine {

struct TaskRunners {
  std::shared_ptr<TaskRunner> platform;
  std::shared_ptr<TaskRunner> ui;
  std::shared_ptr<TaskRunner> raster;
  std::shared_ptr<TaskRunner> io;

  bool IsValid() const { return platform && ui && raster && io; }
};

}  // namespace momo::engine
