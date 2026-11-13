// AppDriver 的 JS 实现: JsRuntime + momo:ui / momo:services 绑定。

#include <utility>

#include "base/logging.h"
#include "engine/runtime/js_runtime.h"
#include "engine/ui/app_driver.h"
#include "engine/ui/ui_bindings.h"

namespace momo::engine::ui {

namespace {

class JsAppDriver final : public AppDriver {
 public:
  JsAppDriver(std::unique_ptr<runtime::JsRuntime> runtime, WindowClient& client,
              const AppHost& host, const Settings& settings)
      : runtime_(std::move(runtime)),
        bindings_(UiBindings::Install(*runtime_, client, host, settings)),
        placeholder_(client) {}

  Status Run(const std::filesystem::path& bundle) { return runtime_->RunMainModule(bundle); }
  runtime::JsRuntime& runtime() { return *runtime_; }

  // bundle 未导入 momo:ui 时没有 window 可驱动: 退回占位行为。
  void OnMetricsChanged(ViewId view) override {
    bindings_->is_imported() ? bindings_->MetricsChanged(view) : placeholder_.OnMetricsChanged(view);
  }
  void BeginFrame(TimePoint frame_start) override {
    if (!bindings_->is_imported()) {
      placeholder_.BeginFrame(frame_start);
      return;
    }
    bindings_->BeginFrame(frame_start);
    bindings_->DrawFrame();
  }
  void OnPointerPacket(ViewId view, const platform::PointerDataPacket& packet) override {
    bindings_->PointerPacket(view, packet);
  }
  bool OnKeyData(ViewId view, const platform::KeyData& data) override {
    return bindings_->KeyData(view, data);
  }
  void OnVisibilityChanged(ViewId view, bool visible) override {
    bindings_->VisibilityChanged(view, visible);
  }
  void OnFocusChanged(ViewId view, bool focused) override {
    bindings_->FocusChanged(view, focused);
  }
  void OnViewAdded(ViewId view) override { bindings_->ViewAdded(view); }
  void OnViewRemoved(ViewId view) override { bindings_->ViewRemoved(view); }

 private:
  // 声明顺序即逆序析构: bindings_ 先于 runtime_ 销毁。
  std::unique_ptr<runtime::JsRuntime> runtime_;
  std::unique_ptr<UiBindings> bindings_;
  PlaceholderDriver placeholder_;
};

}  // namespace

StatusOr<std::unique_ptr<AppDriver>> CreateAppDriver(const Settings& settings,
                                                     WindowClient& client, const AppHost& host) {
  if (settings.bundle_path.empty()) {
    MOMO_LOG(INFO) << "No JS bundle: placeholder frames.";
    return std::unique_ptr<AppDriver>(std::make_unique<PlaceholderDriver>(client));
  }
  runtime::JsRuntime::Options options;
  options.code_cache_dir = settings.code_cache_dir;
  // --dev: Node 的 inspector(docs/04 §7)。监听失败只由 Node 打印警告, 不影响应用启动。
  options.enable_inspector = settings.enable_inspector;
  options.inspector_port = settings.inspector_port;
  options.inspector_wait = settings.inspector_wait;
  options.on_process_exit = host.request_exit;
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<runtime::JsRuntime> runtime,
                        runtime::JsRuntime::Create(options));
  auto driver = std::make_unique<JsAppDriver>(std::move(runtime), client, host, settings);
  MOMO_LOG(INFO) << "Running JS bundle: " << settings.bundle_path.string();
  MOMO_RETURN_IF_ERROR(driver->Run(settings.bundle_path));
  return std::unique_ptr<AppDriver>(std::move(driver));
}

}  // namespace momo::engine::ui
