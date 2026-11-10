#include "engine/graphics/software_rasterizer.h"

#include <cstring>
#include <memory>

#include "engine/graphics/skia/paint_context.h"
#include "include/codec/SkCodec.h"
#include "include/codec/SkPngDecoder.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

namespace momo::engine::graphics {

namespace {

SkImageInfo InfoFor(const PixelBuffer& target) {
  return SkImageInfo::Make(target.width, target.height, kBGRA_8888_SkColorType,
                           kPremul_SkAlphaType);
}

}  // namespace

Status RasterizeLayerTree(LayerTree& tree, const PixelBuffer& target, uint32_t clear_color) {
  if (target.pixels == nullptr || target.width <= 0 || target.height <= 0 ||
      target.row_bytes < static_cast<size_t>(target.width) * 4) {
    return InvalidArgumentError("RasterizeLayerTree: invalid pixel buffer");
  }
  std::unique_ptr<SkCanvas> canvas =
      SkCanvas::MakeRasterDirect(InfoFor(target), target.pixels, target.row_bytes);
  if (canvas == nullptr) {
    return InternalError("RasterizeLayerTree: cannot wrap pixel buffer");
  }
  canvas->clear(static_cast<SkColor>(clear_color));
  if (tree.root() != nullptr) {
    PrerollContext preroll;
    tree.root()->Preroll(preroll);
    PaintContext paint{canvas.get()};
    tree.root()->Paint(paint);
  }
  return Status::Ok();
}

uint32_t OwnedPixelBuffer::GetColor(int x, int y) const {
  OwnedPixelBuffer* self = const_cast<OwnedPixelBuffer*>(this);
  SkPixmap pixmap(InfoFor(self->view()), data_.data(), static_cast<size_t>(width_) * 4);
  return pixmap.getColor(x, y);
}

StatusOr<std::vector<uint8_t>> EncodePng(const PixelBuffer& pixels) {
  SkPixmap pixmap(InfoFor(pixels), pixels.pixels, pixels.row_bytes);
  SkDynamicMemoryWStream stream;
  if (!SkPngEncoder::Encode(&stream, pixmap, {})) {
    return InternalError("PNG encode failed");
  }
  sk_sp<SkData> data = stream.detachAsData();
  const auto* bytes = static_cast<const uint8_t*>(data->data());
  return std::vector<uint8_t>(bytes, bytes + data->size());
}

Status DecodePng(const std::vector<uint8_t>& png, OwnedPixelBuffer* out) {
  sk_sp<SkData> data = SkData::MakeWithoutCopy(png.data(), png.size());
  std::unique_ptr<SkCodec> codec = SkPngDecoder::Decode(data, nullptr);
  if (codec == nullptr) {
    return InvalidArgumentError("not a PNG");
  }
  const SkISize size = codec->dimensions();
  *out = OwnedPixelBuffer(size.width(), size.height());
  PixelBuffer view = out->view();
  if (codec->getPixels(InfoFor(view), view.pixels, view.row_bytes) != SkCodec::kSuccess) {
    return InternalError("PNG decode failed");
  }
  return Status::Ok();
}

}  // namespace momo::engine::graphics
