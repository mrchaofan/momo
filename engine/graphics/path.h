#pragma once

// Path: 可重放的几何路径(值类型, 线程无关)。JS 的 momo:ui Path 与 DisplayList 都使用它。
// 不暴露 Skia 类型;光栅化时由 graphics 内部转换为 SkPath。

#include <cstdint>
#include <vector>

#include "base/geometry.h"

namespace momo::engine::graphics {

class Path {
 public:
  enum class Verb : uint8_t { kMove, kLine, kQuad, kCubic, kClose };
  enum class FillType : uint8_t { kNonZero, kEvenOdd };

  Path& MoveTo(double x, double y);
  Path& LineTo(double x, double y);
  Path& QuadTo(double cx, double cy, double x, double y);
  Path& CubicTo(double c1x, double c1y, double c2x, double c2y, double x, double y);
  Path& Close();
  Path& AddRect(const Rect& rect);
  // 椭圆(用 4 段三次贝塞尔近似, 与 Skia/Flutter 一致的 kappa)。
  Path& AddOval(const Rect& rect);
  Path& AddRRect(const Rect& rect, double radius);
  void Reset();

  void set_fill_type(FillType type) { fill_type_ = type; }
  FillType fill_type() const { return fill_type_; }

  bool empty() const { return verbs_.empty(); }
  const std::vector<Verb>& verbs() const { return verbs_; }
  const std::vector<Point>& points() const { return points_; }

  // 控制点包围盒(保守)。空路径返回空矩形。
  Rect bounds() const;

  friend bool operator==(const Path&, const Path&) = default;

 private:
  void EnsureMoveTo();

  std::vector<Verb> verbs_;
  std::vector<Point> points_;
  FillType fill_type_ = FillType::kNonZero;
  bool needs_move_ = true;
  Point last_move_;
};

}  // namespace momo::engine::graphics
