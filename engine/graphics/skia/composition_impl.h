#pragma once

// graphics 内部头: Composition 的录制结果。只能被 engine/graphics 的实现文件包含(含 Skia 类型)。

#include <vector>

#include "engine/graphics/composition.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkPicture.h"

namespace momo::engine::graphics {

struct Composition::Impl {
  struct Slice {
    sk_sp<SkPicture> picture;           // 设备坐标系下的录制结果(可能为空 = 无内容)
    std::vector<Rect> leaf_rects;       // 叶子的设备空间范围(已与帧求交)
    std::optional<Rect> bounds;         // leaf_rects 的并集(外扩为整数)
  };
  std::vector<Slice> slices;
};

// 把分片画到 canvas: 先清为 clear_color, 再以 origin 为原点(canvas 像素 (0,0) = 设备坐标 origin)重放。
void DrawCompositionSlice(const Composition& composition, size_t slice, SkCanvas* canvas,
                          Point origin, uint32_t clear_color);

}  // namespace momo::engine::graphics
