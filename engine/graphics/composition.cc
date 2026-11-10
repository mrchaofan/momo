#include "engine/graphics/composition.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/graphics/skia/composition_impl.h"
#include "engine/graphics/skia/paint_context.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPictureRecorder.h"
#include "include/utils/SkNWayCanvas.h"

namespace momo::engine::graphics {

namespace {

Rect FromSkRect(const SkRect& r) { return Rect::FromLTRB(r.left(), r.top(), r.right(), r.bottom()); }

// 外扩为整数像素。
Rect RoundOut(const Rect& r) {
  const double l = std::floor(r.x), t = std::floor(r.y);
  return Rect::FromLTRB(l, t, std::ceil(r.right()), std::ceil(r.bottom()));
}

class Slicer final : public ViewSlicer {
 public:
  Slicer(std::vector<SkCanvas*> canvases, std::vector<Composition::Impl::Slice>& slices,
         const Rect& frame)
      : canvases_(std::move(canvases)), slices_(slices), frame_(frame) {}

  void NoteLeafDraw(SkCanvas* leaf, const std::optional<Rect>& local_bounds) override {
    const SkIRect clip = leaf->getDeviceClipBounds();
    if (clip.isEmpty()) {
      return;
    }
    SkRect device = SkRect::Make(clip);
    if (local_bounds) {
      SkRect mapped = leaf->getTotalMatrix().mapRect(ToSkRect(*local_bounds));
      if (!mapped.intersect(device)) {
        return;
      }
      device = mapped;
    }
    std::optional<Rect> rect = FromSkRect(device).Intersect(frame_);
    if (!rect) {
      return;
    }
    std::vector<Rect>& rects = slices_[current_].leaf_rects;
    if (rects.size() >= Composition::kMaxLeafRectsPerSlice) {
      rects.back() = rects.back().Union(*rect);
    } else {
      rects.push_back(*rect);
    }
  }

  SkCanvas* OnPlatformView() override {
    if (current_ + 1 < canvases_.size()) {
      ++current_;
    }
    return canvases_[current_];
  }

 private:
  std::vector<SkCanvas*> canvases_;
  std::vector<Composition::Impl::Slice>& slices_;
  Rect frame_;
  size_t current_ = 0;
};

}  // namespace

Composition::Composition() : impl_(std::make_unique<Impl>()) {}

Composition::~Composition() = default;

std::unique_ptr<Composition> Composition::Build(LayerTree& tree) {
  std::unique_ptr<Composition> c(new Composition());
  c->frame_size_ = tree.frame_size();
  if (tree.root() == nullptr) {
    return c;
  }
  PrerollContext preroll;
  preroll.platform_views = &c->views_;
  tree.root()->Preroll(preroll);
  if (c->views_.empty()) {
    return c;
  }

  const int width = std::max(1, static_cast<int>(std::ceil(c->frame_size_.width)));
  const int height = std::max(1, static_cast<int>(std::ceil(c->frame_size_.height)));
  const SkRect cull = SkRect::MakeWH(static_cast<SkScalar>(width), static_cast<SkScalar>(height));
  const size_t count = c->views_.size() + 1;
  c->impl_->slices.resize(count);
  std::vector<SkPictureRecorder> recorders(count);
  std::vector<SkCanvas*> canvases;
  canvases.reserve(count);
  SkNWayCanvas nway(width, height);
  for (SkPictureRecorder& recorder : recorders) {
    SkCanvas* canvas = recorder.beginRecording(cull);
    canvases.push_back(canvas);
    nway.addCanvas(canvas);
  }
  Slicer slicer(canvases, c->impl_->slices, Rect{0, 0, c->frame_size_.width, c->frame_size_.height});
  PaintContext paint;
  paint.canvas = &nway;
  paint.leaf_canvas = canvases[0];
  paint.slicer = &slicer;
  tree.root()->Paint(paint);
  nway.removeAll();  // 录制画布在 finish 之后失效

  for (size_t i = 0; i < count; ++i) {
    Impl::Slice& slice = c->impl_->slices[i];
    slice.picture = recorders[i].finishRecordingAsPicture();
    for (const Rect& r : slice.leaf_rects) {
      slice.bounds = slice.bounds ? slice.bounds->Union(r) : r;
    }
    if (slice.bounds) {
      slice.bounds = RoundOut(*slice.bounds).Intersect(Rect{0, 0, static_cast<double>(width),
                                                            static_cast<double>(height)});
    }
  }
  return c;
}

std::optional<Rect> Composition::slice_bounds(size_t slice) const {
  return slice < impl_->slices.size() ? impl_->slices[slice].bounds : std::nullopt;
}

const std::vector<Rect>& Composition::slice_leaf_rects(size_t slice) const {
  static const std::vector<Rect> kEmpty;
  return slice < impl_->slices.size() ? impl_->slices[slice].leaf_rects : kEmpty;
}

std::optional<Rect> Composition::VisibleFrame(size_t view_index) const {
  if (view_index >= views_.size()) {
    return std::nullopt;
  }
  const EmbeddedView& view = views_[view_index];
  std::optional<Rect> visible =
      view.device_frame().Intersect(Rect{0, 0, frame_size_.width, frame_size_.height});
  for (const DeviceClip& clip : view.clips) {
    if (!visible) {
      break;
    }
    visible = visible->Intersect(clip.rect);
  }
  return visible;
}

std::vector<Rect> Composition::OccludersAbove(size_t view_index) const {
  std::vector<Rect> out;
  const std::optional<Rect> visible = VisibleFrame(view_index);
  if (!visible) {
    return out;
  }
  // views[i] 之上的分片是 i+1 .. N。
  for (size_t s = view_index + 1; s < impl_->slices.size(); ++s) {
    for (const Rect& r : impl_->slices[s].leaf_rects) {
      if (std::optional<Rect> hit = r.Intersect(*visible)) {
        out.push_back(*hit);
      }
    }
  }
  return out;
}

void DrawCompositionSlice(const Composition& composition, size_t slice, SkCanvas* canvas,
                          Point origin, uint32_t clear_color) {
  canvas->clear(static_cast<SkColor>(clear_color));
  const auto& slices = composition.impl().slices;
  if (slice >= slices.size() || slices[slice].picture == nullptr) {
    return;
  }
  SkAutoCanvasRestore restore(canvas, true);
  canvas->translate(static_cast<SkScalar>(-origin.x), static_cast<SkScalar>(-origin.y));
  canvas->drawPicture(slices[slice].picture);
}

Status Composition::RasterizeSlice(size_t slice, const PixelBuffer& target, Point origin,
                                   uint32_t clear_color) const {
  if (target.pixels == nullptr || target.width <= 0 || target.height <= 0 ||
      target.row_bytes < static_cast<size_t>(target.width) * 4) {
    return InvalidArgumentError("RasterizeSlice: invalid pixel buffer");
  }
  if (slice >= slice_count()) {
    return InvalidArgumentError("RasterizeSlice: no such slice");
  }
  const SkImageInfo info = SkImageInfo::Make(target.width, target.height, kBGRA_8888_SkColorType,
                                             kPremul_SkAlphaType);
  std::unique_ptr<SkCanvas> canvas =
      SkCanvas::MakeRasterDirect(info, target.pixels, target.row_bytes);
  if (canvas == nullptr) {
    return InternalError("RasterizeSlice: cannot wrap pixel buffer");
  }
  DrawCompositionSlice(*this, slice, canvas.get(), origin, clear_color);
  return Status::Ok();
}

}  // namespace momo::engine::graphics
