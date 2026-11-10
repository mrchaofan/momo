#pragma once

// Composition: 含平台视图的一帧的分片结果(docs/05 §9)。raster 线程使用。
//
// Build 对 LayerTree 做一次收集平台视图的 Preroll, 再按绘制顺序把叶子绘制录制到 N+1 个分片:
//   slice 0       底层: 第一个平台视图之前的全部内容(画进主 surface)
//   slice i(≥1)  views[i-1] 之后、views[i] 之前的内容(画进 views[i-1] 之上的 overlay 图层)
// 容器层的变换/裁剪/saveLayer 经 N 路画布同步到所有分片, 因此每个分片都是完整坐标系(设备像素)下
// 的独立画面, 只是只含属于自己的叶子。
// 公共头不含 Skia 类型(docs/10 §1);录制结果在 Impl 中, 由 graphics 内部的光栅器读取。
// 线程约定: 只在 raster 线程(或测试线程)创建与使用。

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "base/geometry.h"
#include "base/macros.h"
#include "base/status.h"
#include "engine/graphics/layer.h"
#include "engine/graphics/layer_tree.h"
#include "engine/graphics/software_rasterizer.h"

namespace momo::engine::graphics {

class Composition {
 public:
  // 每个分片最多保留的独立内容矩形数(超出时并入最后一个, 用于遮挡判断)。
  static constexpr size_t kMaxLeafRectsPerSlice = 64;

  // tree 中没有平台视图时 views() 为空, 也不录制任何分片(调用方应走普通路径或只做"退出合成")。
  static std::unique_ptr<Composition> Build(LayerTree& tree);
  ~Composition();

  // 按 z 序(绘制顺序)。
  const std::vector<EmbeddedView>& views() const { return views_; }
  size_t slice_count() const { return views_.empty() ? 0 : views_.size() + 1; }
  // 分片 i 的内容范围(设备像素, 外扩为整数并与帧求交);没有内容为 nullopt。
  std::optional<Rect> slice_bounds(size_t slice) const;
  // 分片 i 中各叶子的设备空间范围(已与帧求交)。
  const std::vector<Rect>& slice_leaf_rects(size_t slice) const;
  // 位于 views[view_index] 之上、与其可见范围相交的 momo 内容(已与之求交)。
  std::vector<Rect> OccludersAbove(size_t view_index) const;
  // 平台视图的可见范围: 设备 frame ∩ 祖先裁剪 ∩ 帧。完全不可见为 nullopt。
  std::optional<Rect> VisibleFrame(size_t view_index) const;

  // 把分片画进像素缓冲(软件路径/测试)。目标像素 (0,0) 对应设备坐标 origin。
  // 先清为 clear_color(0xAARRGGBB)。
  Status RasterizeSlice(size_t slice, const PixelBuffer& target, Point origin,
                        uint32_t clear_color = 0x00000000) const;

  struct Impl;  // graphics 内部(skia/composition_impl.h)
  const Impl& impl() const { return *impl_; }

 private:
  Composition();

  Size frame_size_;
  std::vector<EmbeddedView> views_;
  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(Composition);
};

}  // namespace momo::engine::graphics
