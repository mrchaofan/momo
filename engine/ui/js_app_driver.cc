// CreateAppDriver 的 JS 实现: 创建 JS 运行时, 安装 momo:ui / momo:services 绑定(UiBindings), 执行 bundle。

#include <utility>

#include "base/logging.h"
#include "engine/runtime/js_runtime.h"
#include "engine/ui/app_driver.h"
#include "engine/ui/ui_bindings.h"

namespace momo::engine::ui {

StatusOr<std::unique_ptr<AppDriver>> CreateAppDriver(const Settings& settings, WindowClient& client,
                                                     const AppHost& host) {
  if (settings.bundle_path.empty()) {
    MOMO_LOG(INFO) << "No JS bundle: placeholder frames.";
    return std::unique_ptr<AppDriver>(std::make_unique<PlaceholderDriver>(client));
  }
  runtime::JsRuntime::Options options;
  options.code_cache_dir = settings.code_cache_dir;
  options.builtin_dir = settings.builtin_dir;
  // --dev: Node 的 inspector(docs/04 §7)。监听失败只由 Node 打印警告, 不影响应用启动。
  options.enable_inspector = settings.enable_inspector;
  options.inspector_port = settings.inspector_port;
  options.inspector_wait = settings.inspector_wait;
  options.on_process_exit = host.request_exit;
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<runtime::JsRuntime> runtime,
                        runtime::JsRuntime::Create(options));
  std::unique_ptr<UiBindings> bindings = UiBindings::Install(std::move(runtime), client, host, settings);
  MOMO_LOG(INFO) << "Running JS bundle: " << settings.bundle_path.string();
  MOMO_RETURN_IF_ERROR(bindings->runtime().RunMainModule(settings.bundle_path));
  return std::unique_ptr<AppDriver>(std::move(bindings));
}

}  // namespace momo::engine::ui
