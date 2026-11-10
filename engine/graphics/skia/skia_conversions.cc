#include <vector>

#include "engine/graphics/skia/paint_context.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkM44.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkGradientShader.h"

namespace momo::engine::graphics {

namespace {

SkTileMode ToSkTileMode(Gradient::TileMode mode) {
  switch (mode) {
    case Gradient::TileMode::kClamp: return SkTileMode::kClamp;
    case Gradient::TileMode::kRepeat: return SkTileMode::kRepeat;
    case Gradient::TileMode::kMirror: return SkTileMode::kMirror;
  }
  return SkTileMode::kClamp;
}

sk_sp<SkShader> ToSkShader(const Gradient& g) {
  if (g.colors.size() < 2) {
    return nullptr;
  }
  std::vector<SkColor> colors(g.colors.begin(), g.colors.end());
  const SkScalar* pos = nullptr;
  std::vector<SkScalar> stops;
  if (g.stops.size() == g.colors.size()) {
    stops.assign(g.stops.begin(), g.stops.end());
    pos = stops.data();
  }
  const int count = static_cast<int>(colors.size());
  switch (g.type) {
    case Gradient::Type::kLinear: {
      const SkPoint pts[2] = {{static_cast<SkScalar>(g.start.x), static_cast<SkScalar>(g.start.y)},
                              {static_cast<SkScalar>(g.end.x), static_cast<SkScalar>(g.end.y)}};
      return SkGradientShader::MakeLinear(pts, colors.data(), pos, count, ToSkTileMode(g.tile_mode));
    }
    case Gradient::Type::kRadial:
      return SkGradientShader::MakeRadial(
          {static_cast<SkScalar>(g.start.x), static_cast<SkScalar>(g.start.y)},
          static_cast<SkScalar>(g.radius), colors.data(), pos, count, ToSkTileMode(g.tile_mode));
  }
  return nullptr;
}

}  // namespace

SkRect ToSkRect(const Rect& r) {
  return SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                          static_cast<SkScalar>(r.width), static_cast<SkScalar>(r.height));
}

SkM44 ToSkM44(const Matrix& m) {
  return SkM44(static_cast<SkScalar>(m.at(0, 0)), static_cast<SkScalar>(m.at(0, 1)),
               static_cast<SkScalar>(m.at(0, 2)), static_cast<SkScalar>(m.at(0, 3)),
               static_cast<SkScalar>(m.at(1, 0)), static_cast<SkScalar>(m.at(1, 1)),
               static_cast<SkScalar>(m.at(1, 2)), static_cast<SkScalar>(m.at(1, 3)),
               static_cast<SkScalar>(m.at(2, 0)), static_cast<SkScalar>(m.at(2, 1)),
               static_cast<SkScalar>(m.at(2, 2)), static_cast<SkScalar>(m.at(2, 3)),
               static_cast<SkScalar>(m.at(3, 0)), static_cast<SkScalar>(m.at(3, 1)),
               static_cast<SkScalar>(m.at(3, 2)), static_cast<SkScalar>(m.at(3, 3)));
}

SkPaint ToSkPaint(const Paint& p) {
  SkPaint paint;
  paint.setColor(static_cast<SkColor>(p.color));
  paint.setAntiAlias(p.anti_alias);
  paint.setStyle(p.style == Paint::Style::kStroke ? SkPaint::kStroke_Style : SkPaint::kFill_Style);
  paint.setStrokeWidth(static_cast<SkScalar>(p.stroke_width));
  switch (p.stroke_cap) {
    case Paint::StrokeCap::kButt: paint.setStrokeCap(SkPaint::kButt_Cap); break;
    case Paint::StrokeCap::kRound: paint.setStrokeCap(SkPaint::kRound_Cap); break;
    case Paint::StrokeCap::kSquare: paint.setStrokeCap(SkPaint::kSquare_Cap); break;
  }
  switch (p.stroke_join) {
    case Paint::StrokeJoin::kMiter: paint.setStrokeJoin(SkPaint::kMiter_Join); break;
    case Paint::StrokeJoin::kRound: paint.setStrokeJoin(SkPaint::kRound_Join); break;
    case Paint::StrokeJoin::kBevel: paint.setStrokeJoin(SkPaint::kBevel_Join); break;
  }
  if (p.gradient) {
    paint.setShader(ToSkShader(*p.gradient));
  }
  if (p.mask_blur_sigma > 0) {
    paint.setMaskFilter(
        SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, static_cast<SkScalar>(p.mask_blur_sigma)));
  }
  return paint;
}

SkSamplingOptions ToSkSampling(Paint::FilterQuality quality) {
  switch (quality) {
    case Paint::FilterQuality::kNone: return SkSamplingOptions(SkFilterMode::kNearest);
    case Paint::FilterQuality::kLow: return SkSamplingOptions(SkFilterMode::kLinear);
    case Paint::FilterQuality::kMedium:
      return SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear);
    case Paint::FilterQuality::kHigh: return SkSamplingOptions(SkCubicResampler::Mitchell());
  }
  return SkSamplingOptions(SkFilterMode::kLinear);
}

SkPath ToSkPath(const Path& path) {
  SkPathBuilder builder(path.fill_type() == Path::FillType::kEvenOdd ? SkPathFillType::kEvenOdd
                                                                     : SkPathFillType::kWinding);
  const auto& pts = path.points();
  size_t i = 0;
  auto pt = [&pts](size_t index) {
    return SkPoint::Make(static_cast<SkScalar>(pts[index].x), static_cast<SkScalar>(pts[index].y));
  };
  for (Path::Verb verb : path.verbs()) {
    switch (verb) {
      case Path::Verb::kMove: builder.moveTo(pt(i)); i += 1; break;
      case Path::Verb::kLine: builder.lineTo(pt(i)); i += 1; break;
      case Path::Verb::kQuad: builder.quadTo(pt(i), pt(i + 1)); i += 2; break;
      case Path::Verb::kCubic: builder.cubicTo(pt(i), pt(i + 1), pt(i + 2)); i += 3; break;
      case Path::Verb::kClose: builder.close(); break;
    }
  }
  return builder.detach();
}

namespace {

// 把指令流重放到 canvas(录制 SkPicture 与动态重放共用)。paints 为预先转换好的 SkPaint。
void ReplayOps(const DisplayList& list, SkCanvas* canvas, const std::vector<SkPaint>& paints) {
  using OpType = DisplayList::OpType;
  auto paint_or_null = [&paints](uint32_t index) -> const SkPaint* {
    return index == DisplayList::kNoPaint ? nullptr : &paints[index];
  };
  auto sampling = [&list](uint32_t index) {
    return ToSkSampling(index == DisplayList::kNoPaint ? Paint::FilterQuality::kLow
                                                       : list.paints()[index].filter_quality);
  };
  for (const DisplayList::Op& op : list.ops()) {
    const float* a = op.a;
    switch (op.type) {
      case OpType::kSave: canvas->save(); break;
      case OpType::kRestore: canvas->restore(); break;
      case OpType::kTranslate: canvas->translate(a[0], a[1]); break;
      case OpType::kScale: canvas->scale(a[0], a[1]); break;
      case OpType::kRotate: canvas->rotate(SkRadiansToDegrees(a[0])); break;
      case OpType::kTransform: canvas->concat(ToSkM44(list.matrices()[op.aux])); break;
      case OpType::kClipRect: canvas->clipRect(SkRect::MakeXYWH(a[0], a[1], a[2], a[3]), true); break;
      case OpType::kClipRRect:
        canvas->clipRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(a[0], a[1], a[2], a[3]), a[4], a[4]),
                          true);
        break;
      case OpType::kClipPath: canvas->clipPath(ToSkPath(*list.paths()[op.aux]), true); break;
      case OpType::kSaveLayer: {
        const SkRect bounds = SkRect::MakeXYWH(a[0], a[1], a[2], a[3]);
        canvas->saveLayer(op.aux != 0 ? &bounds : nullptr, paint_or_null(op.paint));
        break;
      }
      case OpType::kDrawColor: canvas->drawColor(static_cast<SkColor>(op.paint)); break;
      case OpType::kDrawPaint: canvas->drawPaint(paints[op.paint]); break;
      case OpType::kDrawRect:
        canvas->drawRect(SkRect::MakeXYWH(a[0], a[1], a[2], a[3]), paints[op.paint]);
        break;
      case OpType::kDrawRRect:
        canvas->drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(a[0], a[1], a[2], a[3]), a[4], a[4]),
                          paints[op.paint]);
        break;
      case OpType::kDrawOval:
        canvas->drawOval(SkRect::MakeXYWH(a[0], a[1], a[2], a[3]), paints[op.paint]);
        break;
      case OpType::kDrawCircle: canvas->drawCircle(a[0], a[1], a[2], paints[op.paint]); break;
      case OpType::kDrawLine: {
        SkPaint stroke = paints[op.paint];
        stroke.setStyle(SkPaint::kStroke_Style);  // 线段总是描边
        canvas->drawLine(a[0], a[1], a[2], a[3], stroke);
        break;
      }
      case OpType::kDrawPath:
        canvas->drawPath(ToSkPath(*list.paths()[op.aux]), paints[op.paint]);
        break;
      case OpType::kDrawDisplayList:
        RenderDisplayList(*list.display_lists()[op.aux], canvas);
        break;
      case OpType::kDrawImage: {
        const sk_sp<SkImage>& image = list.images()[op.aux]->impl().image;
        if (image) {
          canvas->drawImage(image, a[0], a[1], sampling(op.paint), paint_or_null(op.paint));
        }
        break;
      }
      case OpType::kDrawImageRect: {
        const sk_sp<SkImage>& image = list.images()[op.aux]->impl().image;
        if (image) {
          canvas->drawImageRect(image, ToSkRect(list.rects()[static_cast<size_t>(a[4])]),
                                SkRect::MakeXYWH(a[0], a[1], a[2], a[3]), sampling(op.paint),
                                paint_or_null(op.paint), SkCanvas::kFast_SrcRectConstraint);
        }
        break;
      }
      case OpType::kDrawExternal: {
        const auto& ext = list.externals()[op.aux];
        if (ext && ext->picture) {
          SkAutoCanvasRestore restore(canvas, true);
          canvas->translate(a[0], a[1]);
          canvas->drawPicture(ext->picture);
        }
        break;
      }
      case OpType::kDrawTexture: {
        // 动态内容: 取当前最新帧(本 DisplayList 不进入 SkPicture 缓存)。
        std::shared_ptr<const Image> frame = list.textures()[op.aux]->latest();
        if (frame && frame->impl().image) {
          canvas->drawImageRect(frame->impl().image, SkRect::MakeXYWH(a[0], a[1], a[2], a[3]),
                                SkSamplingOptions(SkFilterMode::kLinear), nullptr);
        }
        break;
      }
    }
  }
}

std::vector<SkPaint> ConvertPaints(const DisplayList& list) {
  std::vector<SkPaint> paints;
  paints.reserve(list.paints().size());
  for (const Paint& paint : list.paints()) {
    paints.push_back(ToSkPaint(paint));
  }
  return paints;
}

// 按指令流录制 SkPicture。每个 Paint 只转换一次。
sk_sp<SkPicture> RecordPicture(const DisplayList& list) {
  // 录制范围不做裁剪: 包围盒只是保守估计, 由 drawPicture 用于整体剔除即可。
  constexpr SkScalar kGiant = 1e9f;
  SkPictureRecorder recorder;
  SkCanvas* canvas = recorder.beginRecording(SkRect::MakeLTRB(-kGiant, -kGiant, kGiant, kGiant));
  ReplayOps(list, canvas, ConvertPaints(list));
  return recorder.finishRecordingAsPicture();
}

}  // namespace

const DisplayList::RasterCache& DisplayList::raster_cache() const {
  std::call_once(raster_cache_once_, [this] {
    auto cache = std::make_shared<RasterCache>();
    if (dynamic_) {
      cache->paints = ConvertPaints(*this);  // 动态列表只缓存转换好的 paint
    } else {
      cache->picture = RecordPicture(*this);
    }
    raster_cache_ = std::move(cache);
  });
  return *raster_cache_;
}

void RenderDisplayListWithAlpha(const DisplayList& list, SkCanvas* canvas, uint8_t alpha) {
  if (alpha == 255) {
    RenderDisplayList(list, canvas);
    return;
  }
  std::vector<SkPaint> paints = list.raster_cache().paints.empty() ? ConvertPaints(list)
                                                                   : list.raster_cache().paints;
  for (SkPaint& paint : paints) {
    paint.setAlphaf(paint.getAlphaf() * (alpha / 255.0f));
  }
  SkAutoCanvasRestore restore(canvas, true);
  ReplayOps(list, canvas, paints);
}

void RenderDisplayList(const DisplayList& list, SkCanvas* canvas) {
  const DisplayList::RasterCache& cache = list.raster_cache();
  if (cache.picture) {
    canvas->drawPicture(cache.picture);
    return;
  }
  SkAutoCanvasRestore restore(canvas, true);
  ReplayOps(list, canvas, cache.paints);
}

}  // namespace momo::engine::graphics
