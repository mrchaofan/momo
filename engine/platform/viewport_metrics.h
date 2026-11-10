#pragma once

#include "base/geometry.h"

namespace momo::engine::platform {

// 窗口内容区的度量。尺寸为物理像素。
struct ViewportMetrics {
  Size physical_size;
  double device_pixel_ratio = 1.0;
  friend bool operator==(const ViewportMetrics&, const ViewportMetrics&) = default;
};

}  // namespace momo::engine::platform
