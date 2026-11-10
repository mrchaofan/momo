#include "engine/assets/image_decoder.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/graphics/skia/paint_context.h"
#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkStream.h"

namespace momo::engine::assets {

namespace {

constexpr int kMaxDimension = 16384;  // 防御畸形/恶意图片导致的超大分配

StatusOr<std::unique_ptr<SkCodec>> MakeCodec(sk_sp<SkData> data) {
  SkCodec::Result result = SkCodec::kSuccess;
  std::unique_ptr<SkCodec> codec =
      SkCodec::MakeFromStream(SkMemoryStream::Make(std::move(data)), &result);
  if (codec == nullptr) {
    return InvalidArgumentError("Unsupported or corrupt image data");
  }
  const SkISize size = codec->dimensions();
  if (size.width() <= 0 || size.height() <= 0 || size.width() > kMaxDimension ||
      size.height() > kMaxDimension) {
    return InvalidArgumentError("Image dimensions out of range");
  }
  return codec;
}

SkImageInfo DecodeInfo(const SkCodec& codec) {
  return codec.getInfo()
      .makeColorType(kN32_SkColorType)
      .makeAlphaType(codec.getInfo().alphaType() == kOpaque_SkAlphaType ? kOpaque_SkAlphaType
                                                                        : kPremul_SkAlphaType)
      .makeColorSpace(nullptr);  // 统一按 sRGB 处理
}

}  // namespace

StatusOr<std::shared_ptr<const graphics::Image>> DecodeImage(const std::vector<uint8_t>& data,
                                                             int target_width,
                                                             int target_height) {
  if (data.empty()) {
    return InvalidArgumentError("Empty image data");
  }
  MOMO_ASSIGN_OR_RETURN(std::unique_ptr<SkCodec> codec,
                        MakeCodec(SkData::MakeWithCopy(data.data(), data.size())));
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(DecodeInfo(*codec))) {
    return InternalError("Out of memory decoding image");
  }
  const SkCodec::Result result = codec->getPixels(bitmap.pixmap());
  if (result != SkCodec::kSuccess && result != SkCodec::kIncompleteInput &&
      result != SkCodec::kErrorInInput) {
    return InvalidArgumentError(std::string("Image decode failed: ") +
                                SkCodec::ResultToString(result));
  }
  bitmap.setImmutable();
  sk_sp<SkImage> image = bitmap.asImage();
  // 目标尺寸: 缩放(高质量)。
  if (target_width > 0 || target_height > 0) {
    const double aspect = static_cast<double>(image->width()) / image->height();
    int w = target_width > 0 ? target_width : static_cast<int>(std::lround(target_height * aspect));
    int h = target_height > 0 ? target_height : static_cast<int>(std::lround(target_width / aspect));
    w = std::clamp(w, 1, kMaxDimension);
    h = std::clamp(h, 1, kMaxDimension);
    if (w != image->width() || h != image->height()) {
      SkBitmap scaled;
      if (!scaled.tryAllocPixels(bitmap.info().makeWH(w, h)) ||
          !image->scalePixels(scaled.pixmap(), SkSamplingOptions(SkCubicResampler::Mitchell()))) {
        return InternalError("Image resize failed");
      }
      scaled.setImmutable();
      image = scaled.asImage();
    }
  }
  return graphics::MakeImage(std::move(image));
}

struct MultiFrameCodec::Impl {
  std::unique_ptr<SkCodec> codec;
  std::vector<SkCodec::FrameInfo> frames;
  int next = 0;
  // 已解码帧(供后续依赖前序帧的帧合成;顺序播放时所依赖的帧总在之前解码过)。
  std::vector<SkBitmap> decoded;
};

StatusOr<std::unique_ptr<MultiFrameCodec>> MultiFrameCodec::Create(std::vector<uint8_t> data) {
  if (data.empty()) {
    return InvalidArgumentError("Empty image data");
  }
  auto impl = std::make_unique<Impl>();
  MOMO_ASSIGN_OR_RETURN(impl->codec, MakeCodec(SkData::MakeWithCopy(data.data(), data.size())));
  impl->frames = impl->codec->getFrameInfo();
  return std::unique_ptr<MultiFrameCodec>(new MultiFrameCodec(std::move(impl)));
}

MultiFrameCodec::MultiFrameCodec(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MultiFrameCodec::~MultiFrameCodec() = default;

int MultiFrameCodec::frame_count() const {
  return std::max<int>(1, static_cast<int>(impl_->frames.size()));
}

int MultiFrameCodec::repetition_count() const { return impl_->codec->getRepetitionCount(); }

StatusOr<MultiFrameCodec::Frame> MultiFrameCodec::DecodeNextFrame() {
  const int index = impl_->next;
  impl_->next = (index + 1) % frame_count();
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(DecodeInfo(*impl_->codec))) {
    return InternalError("Out of memory decoding frame");
  }
  SkCodec::Options options;
  options.fFrameIndex = index;
  int duration = 0;
  if (index < static_cast<int>(impl_->frames.size())) {
    const SkCodec::FrameInfo& info = impl_->frames[index];
    duration = info.fDuration;
    const int required = info.fRequiredFrame;
    if (required != SkCodec::kNoFrame) {
      if (required >= static_cast<int>(impl_->decoded.size()) ||
          impl_->decoded[required].isNull()) {
        return InternalError("Frame decoded out of order");
      }
      if (!impl_->decoded[required].readPixels(bitmap.pixmap())) {
        return InternalError("Cannot copy prior frame");
      }
      options.fPriorFrame = required;
    }
  }
  const SkCodec::Result result = impl_->codec->getPixels(bitmap.pixmap(), &options);
  if (result != SkCodec::kSuccess && result != SkCodec::kIncompleteInput &&
      result != SkCodec::kErrorInInput) {
    return InvalidArgumentError(std::string("Frame decode failed: ") +
                                SkCodec::ResultToString(result));
  }
  bitmap.setImmutable();
  if (impl_->decoded.size() < static_cast<size_t>(frame_count())) {
    impl_->decoded.resize(frame_count());
  }
  impl_->decoded[index] = bitmap;  // 共享像素(不可变)
  Frame frame;
  frame.image = graphics::MakeImage(bitmap.asImage());
  frame.duration_ms = duration;
  return frame;
}

}  // namespace momo::engine::assets
