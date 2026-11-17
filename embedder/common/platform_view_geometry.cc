#include "embedder/common/platform_view_geometry.h"

#include <algorithm>
#include <cmath>

namespace momo::embedder {

using engine::platform::CompositionLayout;
using engine::platform::PlatformViewGeometry;

namespace {

bool Contains(const Rect& r, Point p) { return p.x >= r.x && p.y >= r.y && p.x < r.right() && p.y < r.bottom(); }

}  // namespace

std::optional<Rect> PlatformViewVisibleRect(const PlatformViewGeometry& view) {
  std::optional<Rect> visible = view.frame;
  for (const auto& clip : view.clips) {
    if (!visible) {
      break;
    }
    visible = visible->Intersect(clip.rect);
  }
  if (visible && (visible->width <= 0 || visible->height <= 0)) {
    return std::nullopt;
  }
  return visible;
}

double PlatformViewCornerRadius(const PlatformViewGeometry& view, const Rect& visible) {
  double radius = 0;
  for (const auto& clip : view.clips) {
    if (clip.radius <= 0) {
      continue;
    }
    if (std::abs(clip.rect.x - visible.x) <= 1 && std::abs(clip.rect.y - visible.y) <= 1 &&
        std::abs(clip.rect.right() - visible.right()) <= 1 && std::abs(clip.rect.bottom() - visible.bottom()) <= 1) {
      radius = std::max(radius, clip.radius);
    }
  }
  return radius;
}

int64_t PlatformViewAt(const CompositionLayout& layout, Point point, const std::function<bool(int64_t)>& hosted) {
  for (auto it = layout.views.rbegin(); it != layout.views.rend(); ++it) {  // 自上而下
    const std::optional<Rect> visible = PlatformViewVisibleRect(*it);
    if (!visible || !Contains(*visible, point) || !hosted(it->view_id)) {
      continue;
    }
    const bool occluded =
        std::any_of(it->occluders.begin(), it->occluders.end(), [&](const Rect& r) { return Contains(r, point); });
    if (!occluded) {
      return it->view_id;
    }
  }
  return 0;
}

}  // namespace momo::embedder
