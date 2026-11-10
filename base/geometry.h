#pragma once

// 平台无关几何类型。header-only, 属于 momo_base。

#include <algorithm>
#include <cmath>
#include <optional>

namespace momo {

struct Point {
  double x = 0;
  double y = 0;
  friend bool operator==(const Point&, const Point&) = default;
};

struct Size {
  double width = 0;
  double height = 0;
  bool IsEmpty() const { return width <= 0 || height <= 0; }
  friend bool operator==(const Size&, const Size&) = default;
};

struct Rect {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;

  static Rect FromLTRB(double l, double t, double r, double b) { return {l, t, r - l, b - t}; }

  double right() const { return x + width; }
  double bottom() const { return y + height; }
  Size size() const { return {width, height}; }

  // 负宽高翻转为正。
  Rect Normalized() const {
    return {std::min(x, x + width), std::min(y, y + height), std::abs(width), std::abs(height)};
  }
  Rect Offset(double dx, double dy) const { return {x + dx, y + dy, width, height}; }
  Rect Outset(double d) const { return {x - d, y - d, width + 2 * d, height + 2 * d}; }
  Rect Union(const Rect& o) const {
    return FromLTRB(std::min(x, o.x), std::min(y, o.y), std::max(right(), o.right()),
                    std::max(bottom(), o.bottom()));
  }
  // 不相交(或面积为 0)时为 nullopt。
  std::optional<Rect> Intersect(const Rect& o) const {
    const Rect r = FromLTRB(std::max(x, o.x), std::max(y, o.y), std::min(right(), o.right()),
                            std::min(bottom(), o.bottom()));
    if (r.width <= 0 || r.height <= 0) {
      return std::nullopt;
    }
    return r;
  }

  friend bool operator==(const Rect&, const Rect&) = default;
};

// 可选包围盒(nullopt = 无界)的并集: 任一无界则无界。
inline std::optional<Rect> UnionBounds(const std::optional<Rect>& a, const std::optional<Rect>& b) {
  if (!a || !b) {
    return std::nullopt;
  }
  return a->Union(*b);
}

}  // namespace momo
