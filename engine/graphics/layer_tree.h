#pragma once

// LayerTree: 一帧画面的合成结构(docs/05 §3)。ui 线程产生 → 经 Pipeline 交给 raster 线程消费。
// 线程约定: 入队后不可变(唯一所有权随 Pipeline 移交;其中的 Layer/DisplayList 可被多帧共享, 只读)。
// 坐标: root 的坐标系即物理像素(框架层负责按 device_pixel_ratio 缩放)。

#include <cstdint>
#include <memory>

#include "base/geometry.h"
#include "base/time.h"
#include "engine/graphics/layer.h"

namespace momo::engine::graphics {

class LayerTree {
 public:
  struct FrameTimings {
    TimePoint vsync_start;   // 本帧 vsync 时间
    TimePoint frame_target;  // 期望上屏时间
    TimePoint build_start;   // JS onBeginFrame 开始
    TimePoint build_end;     // 提交 Scene 时刻
  };

  // root 可为空(空帧: 只清屏)。
  LayerTree(Size frame_size, double device_pixel_ratio,
            std::shared_ptr<ContainerLayer> root = nullptr)
      : frame_size_(frame_size), device_pixel_ratio_(device_pixel_ratio), root_(std::move(root)) {}

  // 物理像素。
  Size frame_size() const { return frame_size_; }
  double device_pixel_ratio() const { return device_pixel_ratio_; }
  const std::shared_ptr<ContainerLayer>& root() const { return root_; }

  // 所属视图(窗口)。0 = 主窗口(kImplicitViewId, engine/platform/view.h)。
  int64_t view_id() const { return view_id_; }
  void set_view_id(int64_t view_id) { view_id_ = view_id; }

  // 树中是否含 PlatformViewLayer(SceneBuilder 记录;Rasterizer 据此决定是否走合成路径)。
  bool has_platform_views() const { return has_platform_views_; }
  void set_has_platform_views(bool value) { has_platform_views_ = value; }

  const FrameTimings& timings() const { return timings_; }
  FrameTimings& mutable_timings() { return timings_; }

 private:
  Size frame_size_;
  double device_pixel_ratio_;
  std::shared_ptr<ContainerLayer> root_;
  int64_t view_id_ = 0;
  bool has_platform_views_ = false;
  FrameTimings timings_;
};

}  // namespace momo::engine::graphics
