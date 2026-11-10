// AppDriver 的 JS 实现: JsRuntime + momo:ui / momo:services 绑定。

#include <utility>

#include "base/logging.h"
#include "engine/runtime/inspector.h"
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
  void set_inspector(std::unique_ptr<runtime::Inspector> inspector) {
    inspector_ = std::move(inspector);
  }

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
  bool OnCloseRequested(ViewId view) override { return bindings_->CloseRequested(view); }
  void OnPointerPacket(ViewId view, const platform::PointerDataPacket& packet) override {
    bindings_->PointerPacket(view, packet);
  }
  bool OnKeyData(ViewId view, const platform::KeyData& data) override {
    return bindings_->KeyData(view, data);
  }
  void OnFocusChanged(ViewId view, bool focused) override {
    bindings_->FocusChanged(view, focused);
  }
  void OnViewAdded(ViewId view) override { bindings_->ViewAdded(view); }
  void OnViewRemoved(ViewId view) override { bindings_->ViewRemoved(view); }
  void OnViewCreateResult(uint64_t request, StatusOr<ViewId> result) override {
    bindings_->ViewCreateResult(request, std::move(result));
  }

 private:
  // 声明顺序即逆序析构: bindings_、inspector_ 先于 runtime_ 销毁。
  std::unique_ptr<runtime::JsRuntime> runtime_;
  std::unique_ptr<runtime::Inspector> inspector_;
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
  options.icu_data_path = settings.icu_data_path;
  options.code_cache_dir = settings.code_cache_dir;
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<runtime::JsRuntime> runtime,
                        runtime::JsRuntime::Create(options));
  auto driver = std::make_unique<JsAppDriver>(std::move(runtime), client, host, settings);
  // --dev: V8 Inspector(docs/04 §7)。须在执行主模块前启动, 主模块中的断点才能生效。
  // 端口被占用等失败只记警告, 不影响应用启动。
  if (settings.enable_inspector) {
    runtime::Inspector::Options inspector_options;
    inspector_options.port = settings.inspector_port;
    inspector_options.wait_for_debugger = settings.inspector_wait;
    inspector_options.target_title = settings.bundle_path.filename().string();
    inspector_options.target_url = "file://" + settings.bundle_path.string();
    auto inspector = runtime::Inspector::Start(driver->runtime(), inspector_options);
    if (inspector.ok()) {
      MOMO_LOG(INFO) << "DevTools: " << (*inspector)->devtools_frontend_url();
      driver->set_inspector(std::move(inspector).value());
    } else {
      MOMO_LOG(WARNING) << "Inspector not started: " << inspector.status();
    }
  }
  MOMO_LOG(INFO) << "Running JS bundle: " << settings.bundle_path.string();
  MOMO_RETURN_IF_ERROR(driver->Run(settings.bundle_path));
  return std::unique_ptr<AppDriver>(std::move(driver));
}

}  // namespace momo::engine::ui
