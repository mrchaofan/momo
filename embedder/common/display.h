#pragma once

#include <cstdint>

#include "base/geometry.h"

namespace momo::embedder {

// 显示器信息。bounds/work_area 为逻辑点, 主屏左上原点、y 向下(与 PlatformWindow::screen_frame 同一坐标系)。
struct DisplayInfo {
  uint64_t id = 0;
  Rect bounds;
  Rect work_area;
  double device_pixel_ratio = 1.0;
  double refresh_rate = 60.0;
};

}  // namespace momo::embedder
