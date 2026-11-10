// 无 V8 的构建(sanitizer): CreateAppDriver 总是返回占位驱动。

#include "base/logging.h"
#include "engine/ui/app_driver.h"

namespace momo::engine::ui {

StatusOr<std::unique_ptr<AppDriver>> CreateAppDriver(const Settings& settings,
                                                     WindowClient& client, const AppHost&) {
  MOMO_LOG(INFO) << "Built without a JS runtime (bundle "
                 << (settings.bundle_path.empty() ? "<none>" : settings.bundle_path.string())
                 << " not executed; placeholder frames).";
  return std::unique_ptr<AppDriver>(std::make_unique<PlaceholderDriver>(client));
}

}  // namespace momo::engine::ui
