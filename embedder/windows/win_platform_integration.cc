// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。

#include "embedder/windows/win_platform_integration.h"

// win_util.h 必须先于 platform_integration.h 的使用点生效(#undef CreateWindow)。
#include "embedder/windows/win_util.h"

#include <objbase.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cstdint>
#include <ios>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_native_loop.h"
#include "embedder/windows/win_services.h"
#include "embedder/windows/win_vsync_dwm.h"
#include "embedder/windows/win_window.h"

namespace momo::embedder::windows {

namespace {

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
  return TRUE;
}

// 物理像素矩形 → 逻辑矩形(各显示器按自身 DPR 换算;混合 DPI 下逻辑坐标不构成连续的全局坐标系)。
Rect ToLogicalRect(const RECT& rect, double dpr) {
  return {rect.left / dpr, rect.top / dpr, (rect.right - rect.left) / dpr,
          (rect.bottom - rect.top) / dpr};
}

DisplayInfo DescribeMonitor(HMONITOR monitor, bool* primary) {
  DisplayInfo info;
  info.id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(monitor));
  UINT dpi_x = USER_DEFAULT_SCREEN_DPI;
  UINT dpi_y = USER_DEFAULT_SCREEN_DPI;
  if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0) {
    dpi_x = USER_DEFAULT_SCREEN_DPI;
  }
  info.device_pixel_ratio = static_cast<double>(dpi_x) / USER_DEFAULT_SCREEN_DPI;
  MONITORINFOEXW monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  *primary = false;
  if (::GetMonitorInfoW(monitor, &monitor_info)) {
    info.bounds = ToLogicalRect(monitor_info.rcMonitor, info.device_pixel_ratio);
    info.work_area = ToLogicalRect(monitor_info.rcWork, info.device_pixel_ratio);
    *primary = (monitor_info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    // dmDisplayFrequency 为 0/1 表示"硬件默认", 保持 60。
    if (::EnumDisplaySettingsW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
      info.refresh_rate = static_cast<double>(mode.dmDisplayFrequency);
    }
  }
  return info;
}

class WinPlatformIntegration final : public PlatformIntegration {
 public:
  WinPlatformIntegration() : services_(native_loop_.message_window()) {
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
    StatusOr<std::unique_ptr<WinWindow>> window = WinWindow::Create(spec, delegate);
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
  // 应用级能力: Windows 暂无应用整体隐藏/激活的实现(全部回复 unimplemented)。
  PlatformApp& app() override { return app_; }

  std::vector<DisplayInfo> displays() const override {
    std::vector<HMONITOR> monitors;
    ::EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    std::vector<DisplayInfo> out;
    out.reserve(monitors.size());
    for (HMONITOR monitor : monitors) {
      bool primary = false;
      DisplayInfo info = DescribeMonitor(monitor, &primary);
      if (primary) {
        out.insert(out.begin(), info);  // 约定: 主显示器在列表第一个(Application 取其刷新率)
      } else {
        out.push_back(info);
      }
    }
    return out;
  }

  NativeLoop& native_loop() override { return native_loop_; }

 private:
  WinNativeLoop native_loop_;  // 先于 services_ 构造(services_ 使用其消息窗口)
  WinServices services_;
  PlatformApp app_;
  bool com_initialized_ = false;
};

}  // namespace

std::unique_ptr<PlatformIntegration> CreateWinPlatformIntegration() {
  return std::make_unique<WinPlatformIntegration>();
}

}  // namespace momo::embedder::windows
