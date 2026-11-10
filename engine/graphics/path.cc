#include "engine/graphics/path.h"

#include <algorithm>
#include <cmath>

namespace momo::engine::graphics {

namespace {

// 四分之一圆的三次贝塞尔近似系数。
constexpr double kKappa = 0.5522847498307936;

}  // namespace

void Path::EnsureMoveTo() {
  if (needs_move_) {
    MoveTo(last_move_.x, last_move_.y);
  }
}

Path& Path::MoveTo(double x, double y) {
  verbs_.push_back(Verb::kMove);
  points_.push_back({x, y});
  last_move_ = {x, y};
  needs_move_ = false;
  return *this;
}

Path& Path::LineTo(double x, double y) {
  EnsureMoveTo();
  verbs_.push_back(Verb::kLine);
  points_.push_back({x, y});
  return *this;
}

Path& Path::QuadTo(double cx, double cy, double x, double y) {
  EnsureMoveTo();
  verbs_.push_back(Verb::kQuad);
  points_.push_back({cx, cy});
  points_.push_back({x, y});
  return *this;
}

Path& Path::CubicTo(double c1x, double c1y, double c2x, double c2y, double x, double y) {
  EnsureMoveTo();
  verbs_.push_back(Verb::kCubic);
  points_.push_back({c1x, c1y});
  points_.push_back({c2x, c2y});
  points_.push_back({x, y});
  return *this;
}

Path& Path::Close() {
  if (!verbs_.empty() && verbs_.back() != Verb::kClose) {
    verbs_.push_back(Verb::kClose);
  }
  needs_move_ = true;  // 之后的绘制从上一个 MoveTo 点重新开始
  return *this;
}

Path& Path::AddRect(const Rect& r) {
  MoveTo(r.x, r.y);
  LineTo(r.x + r.width, r.y);
  LineTo(r.x + r.width, r.y + r.height);
  LineTo(r.x, r.y + r.height);
  return Close();
}

Path& Path::AddOval(const Rect& r) {
  const double cx = r.x + r.width / 2;
  const double cy = r.y + r.height / 2;
  const double rx = r.width / 2;
  const double ry = r.height / 2;
  const double kx = rx * kKappa;
  const double ky = ry * kKappa;
  MoveTo(cx + rx, cy);
  CubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
  CubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
  CubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
  CubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
  return Close();
}

Path& Path::AddRRect(const Rect& r, double radius) {
  const double rad = std::clamp(radius, 0.0, std::min(r.width, r.height) / 2);
  if (rad <= 0) {
    return AddRect(r);
  }
  const double k = rad * (1 - kKappa);
  const double l = r.x;
  const double t = r.y;
  const double rt = r.x + r.width;
  const double b = r.y + r.height;
  MoveTo(l + rad, t);
  LineTo(rt - rad, t);
  CubicTo(rt - k, t, rt, t + k, rt, t + rad);
  LineTo(rt, b - rad);
  CubicTo(rt, b - k, rt - k, b, rt - rad, b);
  LineTo(l + rad, b);
  CubicTo(l + k, b, l, b - k, l, b - rad);
  LineTo(l, t + rad);
  CubicTo(l, t + k, l + k, t, l + rad, t);
  return Close();
}

void Path::Reset() {
  verbs_.clear();
  points_.clear();
  needs_move_ = true;
  last_move_ = {};
}

Rect Path::bounds() const {
  if (points_.empty()) {
    return {};
  }
  double min_x = points_[0].x, max_x = points_[0].x;
  double min_y = points_[0].y, max_y = points_[0].y;
  for (const Point& p : points_) {
    min_x = std::min(min_x, p.x);
    max_x = std::max(max_x, p.x);
    min_y = std::min(min_y, p.y);
    max_y = std::max(max_y, p.y);
  }
  return {min_x, min_y, max_x - min_x, max_y - min_y};
}

}  // namespace momo::engine::graphics
