#include "embedder/windows/win_displays.h"

#include <shellscalingapi.h>

#include <chrono>
#include <cmath>

namespace momo::embedder::windows {

namespace {

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
  auto* out = reinterpret_cast<std::vector<WinMonitorInfo>*>(data);
  WinMonitorInfo info;
  info.id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(monitor));
  UINT dpi_x = USER_DEFAULT_SCREEN_DPI;
  UINT dpi_y = USER_DEFAULT_SCREEN_DPI;
  if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0) {
    dpi_x = USER_DEFAULT_SCREEN_DPI;
  }
  info.dpr = static_cast<double>(dpi_x) / USER_DEFAULT_SCREEN_DPI;
  MONITORINFOEXW monitor_info{};
  monitor_info.cbSize = sizeof(monitor_info);
  if (!::GetMonitorInfoW(monitor, &monitor_info)) {
    return TRUE;
  }
  const RECT& r = monitor_info.rcMonitor;
  const RECT& w = monitor_info.rcWork;
  info.physical = Rect::FromLTRB(r.left, r.top, r.right, r.bottom);
  info.physical_work = Rect::FromLTRB(w.left, w.top, w.right, w.bottom);
  info.primary = (monitor_info.dwFlags & MONITORINFOF_PRIMARY) != 0;
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  // dmDisplayFrequency 为 0/1 表示"硬件默认", 保持 60。
  if (::EnumDisplaySettingsW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) {
    info.refresh_rate = static_cast<double>(mode.dmDisplayFrequency);
  }
  out->push_back(info);
  return TRUE;
}

}  // namespace

std::vector<WinMonitorInfo> WinDisplays::Enumerate() {
  std::vector<WinMonitorInfo> monitors;
  ::EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
  return monitors;
}

const WinScreenLayout& WinDisplays::layout() {
  constexpr TimeDelta kMaxAge = std::chrono::seconds(1);
  const TimePoint now = Clock::now();
  if (!layout_ || now - enumerated_at_ > kMaxAge) {
    layout_.emplace(Enumerate());
    enumerated_at_ = now;
  }
  return *layout_;
}

std::vector<DisplayInfo> WinDisplays::displays() {
  std::vector<DisplayInfo> out;
  const WinScreenLayout& l = layout();
  for (const WinScreenLayout::Monitor& m : l.monitors()) {  // 主屏在前
    DisplayInfo info;
    info.id = m.info.id;
    info.bounds = m.logical;
    info.work_area = l.LogicalWorkArea(m);
    info.device_pixel_ratio = m.info.dpr;
    info.refresh_rate = m.info.refresh_rate;
    out.push_back(info);
  }
  return out;
}

POINT WinDisplays::ToPhysical(Point logical) {
  const Point p = layout().ToPhysical(logical);
  return {static_cast<LONG>(std::lround(p.x)), static_cast<LONG>(std::lround(p.y))};
}

}  // namespace momo::embedder::windows
