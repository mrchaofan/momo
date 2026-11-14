#include "embedder/windows/win_platform_integration.h"

// win_util.h 必须先于 platform_integration.h 的使用点生效(#undef CreateWindow)。
#include "embedder/windows/win_util.h"

#include <objbase.h>

#include <cstdint>
#include <ios>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_app.h"
#include "embedder/windows/win_displays.h"
#include "embedder/windows/win_mouse_events.h"
#include "embedder/windows/win_native_loop.h"
#include "embedder/windows/win_services.h"
#include "embedder/windows/win_vsync_dwm.h"
#include "embedder/windows/win_window.h"

namespace momo::embedder::windows {

namespace {

class WinPlatformIntegration final : public PlatformIntegration {
 public:
  explicit WinPlatformIntegration(WinAppObserver* observer)
      : services_(native_loop_.message_window()), app_(observer) {
    // ShellExecuteW 等 Shell API 要求调用线程已初始化 COM(STA)。
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    com_initialized_ = SUCCEEDED(hr);  // S_FALSE(已初始化)也需配对 CoUninitialize
    if (FAILED(hr)) {
      MOMO_LOG(WARNING) << "CoInitializeEx failed: 0x" << std::hex << static_cast<uint32_t>(hr);
    }
  }

  ~WinPlatformIntegration() override {
    if (com_initialized_) {
      ::CoUninitialize();
    }
  }

  StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(const WindowSpec& spec,
                                                         WindowDelegate* delegate) override {
    if (delegate == nullptr) {
      return InvalidArgumentError("CreateWindow requires a delegate");
    }
    StatusOr<std::unique_ptr<WinWindow>> window =
        WinWindow::Create(spec, delegate, WinWindow::Context{mouse_input_, displays_, app_});
    if (!window.ok()) {
      return window.status();
    }
    return std::unique_ptr<PlatformWindow>(std::move(window).value());
  }

  StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource(const PlatformWindow&) override {
    // DwmFlush 是全局的: 不需要(也不持有)窗口。
    return WinVsyncDwm::Create();
  }

  PlatformServices& services() override { return services_; }
  PlatformApp& app() override { return app_; }

  std::vector<DisplayInfo> displays() const override { return displays_.displays(); }

  NativeLoop& native_loop() override { return native_loop_; }

 private:
  WinNativeLoop native_loop_;  // 先于 services_ 构造(services_ 使用其消息窗口)
  WinServices services_;
  WinApp app_;
  // 显示器布局缓存(displays() 为 const 接口, 缓存的刷新不改变可观察状态)。
  mutable WinDisplays displays_;
  // 进程内唯一的指针输入(PointerDispatcher), 注入每个窗口;窗口先于本对象销毁。
  WinMouseInput mouse_input_;
  bool com_initialized_ = false;
};

}  // namespace

std::unique_ptr<PlatformIntegration> CreateWinPlatformIntegration(WinAppObserver* observer) {
  return std::make_unique<WinPlatformIntegration>(observer);
}

}  // namespace momo::embedder::windows
