#include "engine/graphics/layer.h"

#include <cmath>
#include <utility>

#include "engine/graphics/skia/paint_context.h"
#include "include/core/SkM44.h"
#include "include/core/SkRRect.h"

namespace momo::engine::graphics {

namespace {

// 矩阵的 2D 线性部分的平均缩放(圆角半径换算到设备空间用)。
double ApproxScale(const Matrix& m) {
  const double det = m.at(0, 0) * m.at(1, 1) - m.at(0, 1) * m.at(1, 0);
  return std::sqrt(std::abs(det));
}

// 平台视图收集期间压入/弹出一层祖先裁剪。
class ScopedPrerollClip {
 public:
  ScopedPrerollClip(PrerollContext& context, const Rect& clip, double radius)
      : context_(context.platform_views != nullptr ? &context : nullptr) {
    if (context_ != nullptr) {
      context_->clips.push_back(
          {context_->matrix.MapRect(clip), radius * ApproxScale(context_->matrix)});
    }
  }
  ~ScopedPrerollClip() {
    if (context_ != nullptr) {
      context_->clips.pop_back();
    }
  }

 private:
  PrerollContext* context_;
};

}  // namespace

// ---- ContainerLayer ----

void ContainerLayer::Add(std::shared_ptr<Layer> child) {
  if (child != nullptr) {
    children_.push_back(std::move(child));
  }
}

std::optional<Rect> ContainerLayer::PrerollChildren(PrerollContext& context) {
  if (children_.empty()) {
    return Rect{};
  }
  std::optional<Rect> bounds;
  for (size_t i = 0; i < children_.size(); ++i) {
    children_[i]->Preroll(context);  // 即使已知无界也要 preroll 每个子层
    bounds = i == 0 ? children_[i]->paint_bounds()
                    : UnionBounds(bounds, children_[i]->paint_bounds());
  }
  return bounds;
}

void ContainerLayer::PaintChildren(PaintContext& context) const {
  for (const auto& child : children_) {
    child->Paint(context);
  }
}

void ContainerLayer::Preroll(PrerollContext& context) { paint_bounds_ = PrerollChildren(context); }

void ContainerLayer::Paint(PaintContext& context) const { PaintChildren(context); }

// ---- OffsetLayer ----

void OffsetLayer::Preroll(PrerollContext& context) {
  const Matrix saved = context.matrix;
  context.matrix.PreConcat(Matrix::Translate(offset_.x, offset_.y));
  auto child = PrerollChildren(context);
  context.matrix = saved;
  paint_bounds_ = child ? std::optional<Rect>(child->Offset(offset_.x, offset_.y)) : std::nullopt;
}

void OffsetLayer::Paint(PaintContext& context) const {
  SkAutoCanvasRestore restore(context.canvas, true);
  context.canvas->translate(static_cast<SkScalar>(offset_.x), static_cast<SkScalar>(offset_.y));
  PaintChildren(context);
}

// ---- TransformLayer ----

void TransformLayer::Preroll(PrerollContext& context) {
  const Matrix saved = context.matrix;
  context.matrix.PreConcat(matrix_);
  auto child = PrerollChildren(context);
  context.matrix = saved;
  paint_bounds_ = child ? std::optional<Rect>(matrix_.MapRect(*child)) : std::nullopt;
}

void TransformLayer::Paint(PaintContext& context) const {
  SkAutoCanvasRestore restore(context.canvas, true);
  context.canvas->concat(ToSkM44(matrix_));
  PaintChildren(context);
}

// ---- ClipRRectLayer ----

void ClipRRectLayer::Preroll(PrerollContext& context) {
  std::optional<Rect> child;
  {
    ScopedPrerollClip scoped(context, clip_, radius_);
    child = PrerollChildren(context);
  }
  if (!child) {
    paint_bounds_ = clip_;
    return;
  }
  paint_bounds_ = child->Intersect(clip_).value_or(Rect{});
}

void ClipRRectLayer::Paint(PaintContext& context) const {
  SkAutoCanvasRestore restore(context.canvas, true);
  context.canvas->clipRRect(
      SkRRect::MakeRectXY(ToSkRect(clip_), static_cast<SkScalar>(radius_),
                          static_cast<SkScalar>(radius_)),
      true);
  PaintChildren(context);
}

// ---- ClipRectLayer ----

void ClipRectLayer::Preroll(PrerollContext& context) {
  std::optional<Rect> child;
  {
    ScopedPrerollClip scoped(context, clip_, 0);
    child = PrerollChildren(context);
  }
  if (!child) {
    paint_bounds_ = clip_;
    return;
  }
  paint_bounds_ = child->Intersect(clip_).value_or(Rect{});
}

void ClipRectLayer::Paint(PaintContext& context) const {
  SkAutoCanvasRestore restore(context.canvas, true);
  context.canvas->clipRect(ToSkRect(clip_), true);
  PaintChildren(context);
}

// ---- OpacityLayer ----

void OpacityLayer::Preroll(PrerollContext& context) {
  const double saved = context.opacity;
  if (context.platform_views != nullptr) {
    context.opacity *= alpha_ / 255.0;
  }
  paint_bounds_ = PrerollChildren(context);
  context.opacity = saved;
}

void OpacityLayer::Paint(PaintContext& context) const {
  if (alpha_ == 0) {
    return;
  }
  if (alpha_ == 255) {
    PaintChildren(context);
    return;
  }
  // 子树可继承透明度(单子链 + 叶子无自重叠): 把 alpha 下推到绘制, 省掉离屏 saveLayer。
  if (SingleChildInheritsOpacity()) {
    const uint8_t saved = context.inherited_alpha;
    context.inherited_alpha = static_cast<uint8_t>((saved * alpha_ + 127) / 255);
    PaintChildren(context);
    context.inherited_alpha = saved;
    return;
  }
  SkAutoCanvasRestore restore(context.canvas, false);
  if (paint_bounds_) {
    const SkRect bounds = ToSkRect(*paint_bounds_);
    context.canvas->saveLayerAlpha(&bounds, alpha_);
  } else {
    context.canvas->saveLayerAlpha(nullptr, alpha_);
  }
  PaintChildren(context);
}

// ---- PictureLayer ----

void PictureLayer::Preroll(PrerollContext& context) {
  if (picture_ == nullptr) {
    paint_bounds_ = Rect{};
    return;
  }
  const auto& b = picture_->bounds();
  paint_bounds_ = b ? std::optional<Rect>(b->Offset(offset_.x, offset_.y)) : std::nullopt;
}

void PictureLayer::Paint(PaintContext& context) const {
  if (picture_ == nullptr) {
    return;
  }
  SkCanvas* canvas = context.leaf();
  SkAutoCanvasRestore restore(canvas, true);
  canvas->translate(static_cast<SkScalar>(offset_.x), static_cast<SkScalar>(offset_.y));
  if (context.slicer != nullptr) {
    context.slicer->NoteLeafDraw(canvas, picture_->bounds());
    context.slicer->NoteHitRegions(canvas, picture_->hit_regions());
  }
  if (context.inherited_alpha != 255) {
    RenderDisplayListWithAlpha(*picture_, canvas, context.inherited_alpha);
  } else {
    RenderDisplayList(*picture_, canvas);
  }
}

bool PictureLayer::CanInheritOpacity() const {
  if (picture_ == nullptr) {
    return true;
  }
  using OpType = DisplayList::OpType;
  int draws = 0;
  for (const DisplayList::Op& op : picture_->ops()) {
    switch (op.type) {
      case OpType::kSave:
      case OpType::kRestore:
      case OpType::kTranslate:
      case OpType::kScale:
      case OpType::kRotate:
      case OpType::kTransform:
      case OpType::kClipRect:
      case OpType::kClipRRect:
      case OpType::kClipPath:
        break;
      case OpType::kDrawRect:
      case OpType::kDrawRRect:
      case OpType::kDrawOval:
      case OpType::kDrawCircle:
      case OpType::kDrawPath:
      case OpType::kDrawPaint:
        if (picture_->paints()[op.paint].mask_blur_sigma > 0) {
          return false;
        }
        draws++;
        break;
      case OpType::kDrawImage:
      case OpType::kDrawImageRect:
        if (op.paint == DisplayList::kNoPaint) {
          return false;  // 无 paint 可调制
        }
        draws++;
        break;
      default:
        return false;  // 颜色/图层/嵌套/外部内容: 保守处理
    }
    if (draws > 1) {
      return false;
    }
  }
  return true;
}

// ---- TextureLayer ----

void TextureLayer::Preroll(PrerollContext&) { paint_bounds_ = rect_; }

void TextureLayer::Paint(PaintContext& context) const {
  std::shared_ptr<const Image> frame = texture_ ? texture_->latest() : nullptr;
  if (frame == nullptr || !frame->impl().image) {
    return;
  }
  SkCanvas* canvas = context.leaf();
  if (context.slicer != nullptr) {
    context.slicer->NoteLeafDraw(canvas, rect_);
  }
  SkPaint paint;
  paint.setAlpha(context.inherited_alpha);
  canvas->drawImageRect(frame->impl().image, ToSkRect(rect_),
                        SkSamplingOptions(SkFilterMode::kLinear), &paint);
}

// ---- PlatformViewLayer ----

void PlatformViewLayer::Preroll(PrerollContext& context) {
  paint_bounds_ = rect_;
  if (context.platform_views != nullptr) {
    context.platform_views->push_back(
        {view_id_, rect_, context.matrix, context.clips, context.opacity});
  }
}

void PlatformViewLayer::Paint(PaintContext& context) const {
  // 视图内容由平台绘制;合成路径上, 之后的叶子画进位于本视图之上的下一个分片。
  if (context.slicer != nullptr) {
    context.leaf_canvas = context.slicer->OnPlatformView();
  }
}

}  // namespace momo::engine::graphics
