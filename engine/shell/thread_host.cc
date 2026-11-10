#include "engine/shell/thread_host.h"

#include <utility>

#include "base/logging.h"

namespace momo::engine {

ThreadHost::ThreadHost(std::string label)
    : label_(std::move(label)),
      ui_(label_ + ".ui"),
      raster_(label_ + ".raster"),
      io_(label_ + ".io") {}

ThreadHost::~ThreadHost() { Join(); }

TaskRunners ThreadHost::MakeTaskRunners(std::shared_ptr<TaskRunner> platform) const {
  MOMO_CHECK(platform != nullptr) << "ThreadHost requires a platform task runner";
  MOMO_CHECK(!joined_) << "MakeTaskRunners after Join";
  TaskRunners runners;
  runners.label = label_;
  runners.platform = std::move(platform);
  runners.ui = ui_.GetTaskRunner();
  runners.raster = raster_.GetTaskRunner();
  runners.io = io_.GetTaskRunner();
  return runners;
}

void ThreadHost::Join() {
  if (joined_) {
    return;
  }
  ui_.Join();
  raster_.Join();
  io_.Join();
  joined_ = true;
}

}  // namespace momo::engine
