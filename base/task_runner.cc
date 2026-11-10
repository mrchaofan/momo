#include "base/task_runner.h"

#include <memory>

#include "base/object.h"

namespace momo {

void TaskRunner::ScheduleDeleteLater(Object* object) {
  // 非 EventLoop 的 runner 没有"迭代"概念: 退化为普通任务。闭包被丢弃时对象随之析构。
  PostTask([owned = std::unique_ptr<Object>(object)]() mutable { owned.reset(); });
}

}  // namespace momo
