#include <cstring>
#include <utility>

#include "engine/graphics/image.h"
#include "engine/graphics/skia/paint_context.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"

namespace momo::engine::graphics {

Image::Image(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Image::~Image() = default;

int Image::width() const { return impl_->image ? impl_->image->width() : 0; }
int Image::height() const { return impl_->image ? impl_->image->height() : 0; }
size_t Image::byte_size() const {
  return impl_->image ? static_cast<size_t>(impl_->image->width()) * impl_->image->height() * 4 : 0;
}

std::shared_ptr<const Image> MakeImage(sk_sp<SkImage> image) {
  if (image == nullptr) {
    return nullptr;
  }
  auto impl = std::make_unique<Image::Impl>();
  impl->image = std::move(image);
  return std::make_shared<const Image>(std::move(impl));
}

StatusOr<std::shared_ptr<const Image>> Image::FromRgba(const uint8_t* pixels, int width, int height,
                                                       size_t row_bytes) {
  if (pixels == nullptr || width <= 0 || height <= 0 || width > 32768 || height > 32768) {
    return InvalidArgumentError("Image::FromRgba: invalid pixels or dimensions");
  }
  const size_t tight = static_cast<size_t>(width) * 4;
  if (row_bytes == 0) {
    row_bytes = tight;
  }
  if (row_bytes < tight) {
    return InvalidArgumentError("Image::FromRgba: row_bytes too small");
  }
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  sk_sp<SkData> data = SkData::MakeUninitialized(tight * height);
  auto* dst = static_cast<uint8_t*>(data->writable_data());
  for (int y = 0; y < height; ++y) {
    std::memcpy(dst + tight * y, pixels + row_bytes * y, tight);
  }
  sk_sp<SkImage> image = SkImages::RasterFromData(info, std::move(data), tight);
  if (image == nullptr) {
    return InternalError("Image::FromRgba: cannot create image");
  }
  return MakeImage(std::move(image));
}

}  // namespace momo::engine::graphics
