#include "engine/graphics/display_list.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace momo::engine::graphics {

DisplayListBuilder::DisplayListBuilder(std::optional<Rect> cull_rect)
    : cull_rect_(cull_rect), list_(new DisplayList()) {}

uint32_t DisplayListBuilder::AddPaint(const Paint& paint) {
  auto& paints = list_->paints_;
  // 连续绘制常复用同一 Paint: 只与上一个比较即可去掉大部分重复。
  if (!paints.empty() && paints.back() == paint) {
    return static_cast<uint32_t>(paints.size() - 1);
  }
  paints.push_back(paint);
  return static_cast<uint32_t>(paints.size() - 1);
}

void DisplayListBuilder::Push(DisplayList::OpType type, uint32_t paint, uint32_t aux,
                              std::initializer_list<double> args) {
  DisplayList::Op op{type, paint, aux};
  std::transform(args.begin(), args.end(), op.a, [](double v) { return static_cast<float>(v); });
  list_->ops_.push_back(op);
}

void DisplayListBuilder::AccumulateDevice(const Rect& device) {
  std::optional<Rect> clipped = device;
  if (state_.clip) {
    clipped = clipped->Intersect(*state_.clip);
  }
  if (clipped && cull_rect_) {
    clipped = clipped->Intersect(*cull_rect_);
  }
  if (clipped) {
    bounds_ = bounds_ ? bounds_->Union(*clipped) : *clipped;
  }
}

std::optional<Rect> DisplayListBuilder::ToRoot(const Rect& local) const {
  std::optional<Rect> r = state_.matrix.MapRect(local.Normalized());
  if (r && state_.clip) r = r->Intersect(*state_.clip);
  if (r && cull_rect_) r = r->Intersect(*cull_rect_);
  return r;
}

void DisplayListBuilder::AddHitRegion(const Rect& rect) {
  if (std::optional<Rect> r = ToRoot(rect)) {
    list_->hit_regions_.push_back(*r);
  }
}

void DisplayListBuilder::AccumulateLocal(const Rect& local, const Paint* paint) {
  Rect r = local.Normalized();
  if (paint != nullptr && paint->style == Paint::Style::kStroke) {
    // 描边外扩半个线宽(斜接角最坏情况再放宽, 保守即可)。hairline 至少 1 像素。
    r = r.Outset(std::max(paint->stroke_width, 1.0));
  }
  AccumulateDevice(state_.matrix.MapRect(r));
}

void DisplayListBuilder::AccumulateUnbounded() {
  if (state_.clip) {
    AccumulateDevice(*state_.clip);
  } else if (cull_rect_) {
    AccumulateDevice(*cull_rect_);
  } else {
    unbounded_ = true;
  }
}

void DisplayListBuilder::Save() {
  stack_.push_back(state_);
  Push(DisplayList::OpType::kSave);
}

void DisplayListBuilder::Restore() {
  if (stack_.empty()) {
    return;
  }
  state_ = stack_.back();
  stack_.pop_back();
  Push(DisplayList::OpType::kRestore);
}

void DisplayListBuilder::Translate(double dx, double dy) {
  state_.matrix.PreConcat(Matrix::Translate(dx, dy));
  Push(DisplayList::OpType::kTranslate, 0, 0, {dx, dy});
}

void DisplayListBuilder::Scale(double sx, double sy) {
  state_.matrix.PreConcat(Matrix::Scale(sx, sy));
  Push(DisplayList::OpType::kScale, 0, 0, {sx, sy});
}

void DisplayListBuilder::Rotate(double radians) {
  state_.matrix.PreConcat(Matrix::RotateZ(radians));
  Push(DisplayList::OpType::kRotate, 0, 0, {radians});
}

void DisplayListBuilder::Transform(const Matrix& matrix) {
  state_.matrix.PreConcat(matrix);
  list_->matrices_.push_back(matrix);
  Push(DisplayList::OpType::kTransform, 0,
        static_cast<uint32_t>(list_->matrices_.size() - 1));
}

void DisplayListBuilder::ClipRectState(const Rect& rect) {
  const Rect device = state_.matrix.MapRect(rect.Normalized());
  if (state_.clip) {
    state_.clip = state_.clip->Intersect(device).value_or(Rect{device.x, device.y, 0, 0});
  } else {
    state_.clip = device;
  }
}

void DisplayListBuilder::ClipRect(const Rect& rect) {
  ClipRectState(rect);
  Push(DisplayList::OpType::kClipRect, 0, 0, {rect.x, rect.y, rect.width, rect.height});
}

void DisplayListBuilder::ClipRRect(const Rect& rect, double radius) {
  ClipRectState(rect);
  Push(DisplayList::OpType::kClipRRect, 0, 0, {rect.x, rect.y, rect.width, rect.height, radius});
}

void DisplayListBuilder::ClipPath(std::shared_ptr<const Path> path) {
  if (path == nullptr) {
    return;
  }
  ClipRectState(path->bounds());
  list_->paths_.push_back(std::move(path));
  Push(DisplayList::OpType::kClipPath, 0, static_cast<uint32_t>(list_->paths_.size() - 1));
}

void DisplayListBuilder::SaveLayer(const std::optional<Rect>& bounds, const Paint* paint) {
  stack_.push_back(state_);
  const uint32_t paint_index = paint != nullptr ? AddPaint(*paint) : DisplayList::kNoPaint;
  if (bounds) {
    Push(DisplayList::OpType::kSaveLayer, paint_index, 1,
         {bounds->x, bounds->y, bounds->width, bounds->height});
  } else {
    Push(DisplayList::OpType::kSaveLayer, paint_index, 0);
  }
}

void DisplayListBuilder::DrawColor(uint32_t argb) {
  AccumulateUnbounded();
  Push(DisplayList::OpType::kDrawColor, argb);
}

void DisplayListBuilder::DrawPaint(const Paint& paint) {
  AccumulateUnbounded();
  Push(DisplayList::OpType::kDrawPaint, AddPaint(paint));
}

void DisplayListBuilder::DrawRect(const Rect& r, const Paint& paint) {
  AccumulateLocal(r, &paint);
  Push(DisplayList::OpType::kDrawRect, AddPaint(paint), 0, {r.x, r.y, r.width, r.height});
}

void DisplayListBuilder::DrawRRect(const Rect& r, double radius, const Paint& paint) {
  AccumulateLocal(r, &paint);
  Push(DisplayList::OpType::kDrawRRect, AddPaint(paint), 0,
        {r.x, r.y, r.width, r.height, radius});
}

void DisplayListBuilder::DrawOval(const Rect& r, const Paint& paint) {
  AccumulateLocal(r, &paint);
  Push(DisplayList::OpType::kDrawOval, AddPaint(paint), 0, {r.x, r.y, r.width, r.height});
}

void DisplayListBuilder::DrawCircle(Point c, double radius, const Paint& paint) {
  AccumulateLocal({c.x - radius, c.y - radius, 2 * radius, 2 * radius}, &paint);
  Push(DisplayList::OpType::kDrawCircle, AddPaint(paint), 0, {c.x, c.y, radius});
}

void DisplayListBuilder::DrawLine(Point p0, Point p1, const Paint& paint) {
  Paint stroke = paint;
  stroke.style = Paint::Style::kStroke;  // 线段总是描边
  AccumulateLocal({p0.x, p0.y, p1.x - p0.x, p1.y - p0.y}, &stroke);
  Push(DisplayList::OpType::kDrawLine, AddPaint(paint), 0, {p0.x, p0.y, p1.x, p1.y});
}

void DisplayListBuilder::DrawPath(std::shared_ptr<const Path> path, const Paint& paint) {
  if (path == nullptr) {
    return;
  }
  if (!path->empty()) {
    AccumulateLocal(path->bounds(), &paint);
  }
  list_->paths_.push_back(std::move(path));
  Push(DisplayList::OpType::kDrawPath, AddPaint(paint),
        static_cast<uint32_t>(list_->paths_.size() - 1));
}

void DisplayListBuilder::DrawDisplayList(std::shared_ptr<const DisplayList> display_list) {
  if (display_list == nullptr) {
    return;
  }
  if (display_list->bounds()) {
    AccumulateLocal(*display_list->bounds(), nullptr);
  } else {
    AccumulateUnbounded();
  }
  list_->dynamic_ = list_->dynamic_ || display_list->is_dynamic();
  for (const Rect& r : display_list->hit_regions()) {
    if (std::optional<Rect> mapped = ToRoot(r)) list_->hit_regions_.push_back(*mapped);
  }
  list_->display_lists_.push_back(std::move(display_list));
  Push(DisplayList::OpType::kDrawDisplayList, 0,
        static_cast<uint32_t>(list_->display_lists_.size() - 1));
}

void DisplayListBuilder::DrawDRRect(const Rect& outer, double outer_radius, const Rect& inner,
                                    double inner_radius, const Paint& paint) {
  Path path;
  path.set_fill_type(Path::FillType::kEvenOdd);
  path.AddRRect(outer, outer_radius);
  path.AddRRect(inner, inner_radius);
  DrawPath(path, paint);
}

void DisplayListBuilder::DrawImage(std::shared_ptr<const Image> image, Point offset,
                                   const Paint* paint) {
  if (image == nullptr) {
    return;
  }
  AccumulateLocal({offset.x, offset.y, static_cast<double>(image->width()),
                   static_cast<double>(image->height())},
                  nullptr);
  list_->images_.push_back(std::move(image));
  Push(DisplayList::OpType::kDrawImage, paint ? AddPaint(*paint) : DisplayList::kNoPaint,
       static_cast<uint32_t>(list_->images_.size() - 1), {offset.x, offset.y});
}

void DisplayListBuilder::DrawImageRect(std::shared_ptr<const Image> image, const Rect& src,
                                       const Rect& dst, const Paint* paint) {
  if (image == nullptr) {
    return;
  }
  AccumulateLocal(dst, nullptr);
  list_->images_.push_back(std::move(image));
  list_->rects_.push_back(src);
  Push(DisplayList::OpType::kDrawImageRect, paint ? AddPaint(*paint) : DisplayList::kNoPaint,
       static_cast<uint32_t>(list_->images_.size() - 1),
       {dst.x, dst.y, dst.width, dst.height, static_cast<double>(list_->rects_.size() - 1)});
}

void DisplayListBuilder::DrawExternal(std::shared_ptr<const ExternalPicture> picture, Point offset,
                                      const Rect& local_bounds) {
  if (picture == nullptr) {
    return;
  }
  AccumulateLocal(local_bounds.Offset(offset.x, offset.y), nullptr);
  list_->externals_.push_back(std::move(picture));
  Push(DisplayList::OpType::kDrawExternal, 0,
       static_cast<uint32_t>(list_->externals_.size() - 1), {offset.x, offset.y});
}

void DisplayListBuilder::DrawTexture(std::shared_ptr<Texture> texture, const Rect& dst) {
  if (texture == nullptr) {
    return;
  }
  AccumulateLocal(dst, nullptr);
  list_->dynamic_ = true;
  list_->textures_.push_back(std::move(texture));
  Push(DisplayList::OpType::kDrawTexture, 0, static_cast<uint32_t>(list_->textures_.size() - 1),
       {dst.x, dst.y, dst.width, dst.height});
}

std::shared_ptr<const DisplayList> DisplayListBuilder::Build() {
  while (!stack_.empty()) {
    Restore();
  }
  list_->bounds_ = unbounded_ ? std::nullopt : (bounds_ ? bounds_ : std::optional<Rect>(Rect{}));
  std::shared_ptr<const DisplayList> result(list_.release());
  list_.reset(new DisplayList());
  state_ = {};
  bounds_.reset();
  unbounded_ = false;
  return result;
}

}  // namespace momo::engine::graphics
