#pragma once

// 图片解码(docs/02 §6、docs/05 §6): Skia codecs(PNG/JPEG/WebP/GIF/BMP/ICO)。
// 解码产物为不可变的 graphics::Image(CPU 位图), raster 线程首次绘制时上传 GPU。
// 线程约定: DecodeImage 无状态, 任意线程(通常 io 线程)调用;
// MultiFrameCodec 内部持有解码状态, 只能在同一线程(io)上顺序调用 DecodeNextFrame, 可跨线程转移所有权。

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/status.h"
#include "engine/graphics/image.h"

namespace momo::engine::assets {

// target_width/height > 0 时缩放到该尺寸(另一维为 0 时按比例)。
StatusOr<std::shared_ptr<const graphics::Image>> DecodeImage(const std::vector<uint8_t>& data,
                                                             int target_width = 0,
                                                             int target_height = 0);

class MultiFrameCodec {
 public:
  struct Impl;
  struct Frame {
    std::shared_ptr<const graphics::Image> image;
    int duration_ms = 0;
  };

  static StatusOr<std::unique_ptr<MultiFrameCodec>> Create(std::vector<uint8_t> data);
  ~MultiFrameCodec();

  int frame_count() const;
  int repetition_count() const;  // -1 = 无限
  // 解码下一帧(末尾后回到第 0 帧)。
  StatusOr<Frame> DecodeNextFrame();

 private:
  explicit MultiFrameCodec(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(MultiFrameCodec);
};

}  // namespace momo::engine::assets
