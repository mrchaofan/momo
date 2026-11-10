#pragma once

// Paint: 绘制样式(值类型)。颜色一律为 0xAARRGGBB(非预乘)。docs/05 §2。

#include <cstdint>
#include <memory>
#include <vector>

#include "base/geometry.h"

namespace momo::engine::graphics {

// 渐变着色器(不可变, 可在 Paint 间共享)。
struct Gradient {
  enum class Type : uint8_t { kLinear, kRadial };
  enum class TileMode : uint8_t { kClamp, kRepeat, kMirror };

  Type type = Type::kLinear;
  Point start;        // linear: 起点;radial: 圆心
  Point end;          // linear: 终点
  double radius = 0;  // radial
  std::vector<uint32_t> colors;
  std::vector<float> stops;  // 空 = 均匀分布;否则与 colors 等长, 单调递增于 [0,1]
  TileMode tile_mode = TileMode::kClamp;

  friend bool operator==(const Gradient&, const Gradient&) = default;
};

struct Paint {
  enum class Style : uint8_t { kFill, kStroke };
  enum class StrokeCap : uint8_t { kButt, kRound, kSquare };
  enum class StrokeJoin : uint8_t { kMiter, kRound, kBevel };
  enum class FilterQuality : uint8_t { kNone, kLow, kMedium, kHigh };  // 图片采样

  uint32_t color = 0xFF000000;
  Style style = Style::kFill;
  double stroke_width = 0;  // 0 = hairline
  StrokeCap stroke_cap = StrokeCap::kButt;
  StrokeJoin stroke_join = StrokeJoin::kMiter;
  bool anti_alias = true;
  FilterQuality filter_quality = FilterQuality::kLow;
  double mask_blur_sigma = 0;  // > 0: 高斯模糊遮罩(阴影)
  std::shared_ptr<const Gradient> gradient;  // 非空时覆盖 color 的 RGB(alpha 仍调制)

  friend bool operator==(const Paint& a, const Paint& b) {
    const bool same_gradient = a.gradient == b.gradient ||
                               (a.gradient && b.gradient && *a.gradient == *b.gradient);
    return a.color == b.color && a.style == b.style && a.stroke_width == b.stroke_width &&
           a.stroke_cap == b.stroke_cap && a.stroke_join == b.stroke_join &&
           a.anti_alias == b.anti_alias && a.filter_quality == b.filter_quality &&
           a.mask_blur_sigma == b.mask_blur_sigma && same_gradient;
  }
};

}  // namespace momo::engine::graphics
