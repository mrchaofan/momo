#pragma once

// 合成层(docs/05 §3)。ui 线程由 SceneBuilder 组装, 构建后结构与属性不可变, 随 LayerTree 交给 raster 线程。
//   Layer(抽象: Preroll / Paint)
//   ├── ContainerLayer
//   │   ├── OffsetLayer      纯平移(RepaintBoundary 对应物)
//   │   ├── TransformLayer   4x4 矩阵
//   │   ├── ClipRectLayer / ClipRRectLayer
//   │   └── OpacityLayer     alpha 0..255;子树可"继承透明度"时下推到绘制, 否则 saveLayer
//   ├── PictureLayer         叶子: DisplayList + 偏移
//   ├── TextureLayer         叶子: 外部纹理(扩展注入)的最新帧
//   └── PlatformViewLayer    叶子: 平台视图(系统原生视图)占位;自身不绘制, 合成时把画面切片(docs/05 §9)
// Preroll 计算每层在父坐标系下的 paint_bounds(供后续脏区/缓存决策);Paint 由 graphics 内部
// 的光栅器调用(PaintContext 只在 graphics 实现中定义, 公共头不暴露 Skia 类型)。
// 唯一的可变状态是 paint_bounds_: 只由 raster 线程在 Preroll 中写入。同一 Layer 可被多帧共享,
// 但只能在 raster 线程上串行 Preroll/Paint(ui 线程不读它)。

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "base/geometry.h"
#include "base/matrix.h"
#include "engine/graphics/display_list.h"

namespace momo::engine::graphics {

struct PaintContext;  // graphics 内部(skia/paint_context.h)

// 祖先裁剪(设备空间轴对齐矩形 + 圆角半径;含旋转/斜切时为包围盒近似)。
struct DeviceClip {
  Rect rect;
  double radius = 0;
};

// Preroll 收集到的一个平台视图(按绘制顺序)。
struct EmbeddedView {
  int64_t view_id = 0;
  Rect rect;                       // 本地坐标
  Matrix transform;                // 本地 → 设备
  std::vector<DeviceClip> clips;   // 从外到内
  double opacity = 1.0;            // 祖先 OpacityLayer 累计(0..1)

  Rect device_frame() const { return transform.MapRect(rect); }
  // 变换只含平移与正向缩放(无旋转/斜切/透视)。
  bool axis_aligned() const {
    return transform.IsAffine() && transform.at(0, 1) == 0 && transform.at(1, 0) == 0 &&
           transform.at(0, 0) > 0 && transform.at(1, 1) > 0;
  }
};

struct PrerollContext {
  Matrix matrix;  // 当前层到设备的变换
  // 非空时收集平台视图(合成路径, docs/05 §9);同时维护下面两项祖先状态。
  std::vector<EmbeddedView>* platform_views = nullptr;
  std::vector<DeviceClip> clips;  // 当前祖先裁剪栈(仅收集时维护)
  double opacity = 1.0;           // 当前祖先透明度(仅收集时维护)
};

class Layer {
 public:
  virtual ~Layer() = default;

  virtual void Preroll(PrerollContext& context) = 0;
  virtual void Paint(PaintContext& context) const = 0;

  // 父坐标系下的绘制范围;nullopt = 无界。Preroll 之后有效。
  const std::optional<Rect>& paint_bounds() const { return paint_bounds_; }

  // 本层能否把父 OpacityLayer 的 alpha 直接调制到自身绘制上(无自身重叠), 从而省掉 saveLayer
  // (docs/05 §3)。为 true 时 Paint 会读取 PaintContext::inherited_alpha。
  virtual bool CanInheritOpacity() const { return false; }

 protected:
  std::optional<Rect> paint_bounds_;
};

class ContainerLayer : public Layer {
 public:
  void Add(std::shared_ptr<Layer> child);
  const std::vector<std::shared_ptr<Layer>>& children() const { return children_; }

  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;

 protected:
  // 子层包围盒的并集(子坐标系即本层坐标系)。
  std::optional<Rect> PrerollChildren(PrerollContext& context);
  void PaintChildren(PaintContext& context) const;
  bool SingleChildInheritsOpacity() const {
    return children_.size() == 1 && children_[0]->CanInheritOpacity();
  }

 private:
  std::vector<std::shared_ptr<Layer>> children_;
};

class OffsetLayer final : public ContainerLayer {
 public:
  explicit OffsetLayer(Point offset) : offset_(offset) {}
  Point offset() const { return offset_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;
  bool CanInheritOpacity() const override { return SingleChildInheritsOpacity(); }

 private:
  Point offset_;
};

class TransformLayer final : public ContainerLayer {
 public:
  explicit TransformLayer(const Matrix& matrix) : matrix_(matrix) {}
  const Matrix& matrix() const { return matrix_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;
  bool CanInheritOpacity() const override { return SingleChildInheritsOpacity(); }

 private:
  Matrix matrix_;
};

class ClipRRectLayer final : public ContainerLayer {
 public:
  ClipRRectLayer(const Rect& clip, double radius) : clip_(clip), radius_(radius) {}
  const Rect& clip() const { return clip_; }
  double radius() const { return radius_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;
  bool CanInheritOpacity() const override { return SingleChildInheritsOpacity(); }

 private:
  Rect clip_;
  double radius_;
};

class ClipRectLayer final : public ContainerLayer {
 public:
  explicit ClipRectLayer(const Rect& clip) : clip_(clip) {}
  const Rect& clip() const { return clip_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;
  bool CanInheritOpacity() const override { return SingleChildInheritsOpacity(); }

 private:
  Rect clip_;
};

class OpacityLayer final : public ContainerLayer {
 public:
  explicit OpacityLayer(uint8_t alpha) : alpha_(alpha) {}
  uint8_t alpha() const { return alpha_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;

 private:
  uint8_t alpha_;
};

class PictureLayer final : public Layer {
 public:
  PictureLayer(Point offset, std::shared_ptr<const DisplayList> picture)
      : offset_(offset), picture_(std::move(picture)) {}
  Point offset() const { return offset_; }
  const std::shared_ptr<const DisplayList>& picture() const { return picture_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;
  // 只含一条(不自重叠的)绘制指令时可继承透明度。
  bool CanInheritOpacity() const override;

 private:
  Point offset_;
  std::shared_ptr<const DisplayList> picture_;
};

// 平台视图占位: rect 为视图在本层坐标系中的位置与大小。Paint 不画任何像素;在合成路径上
// (PaintContext::slicer 非空)它把之后的叶子绘制切到下一个分片。
class PlatformViewLayer final : public Layer {
 public:
  PlatformViewLayer(const Rect& rect, int64_t view_id) : rect_(rect), view_id_(view_id) {}
  const Rect& rect() const { return rect_; }
  int64_t view_id() const { return view_id_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;
  // 透明度由平台视图自身(embedder)应用, 不需要 saveLayer。
  bool CanInheritOpacity() const override { return true; }

 private:
  Rect rect_;
  int64_t view_id_;
};

class TextureLayer final : public Layer {
 public:
  TextureLayer(const Rect& rect, std::shared_ptr<Texture> texture)
      : rect_(rect), texture_(std::move(texture)) {}
  const Rect& rect() const { return rect_; }
  void Preroll(PrerollContext& context) override;
  void Paint(PaintContext& context) const override;
  bool CanInheritOpacity() const override { return true; }

 private:
  Rect rect_;
  std::shared_ptr<Texture> texture_;
};

}  // namespace momo::engine::graphics
