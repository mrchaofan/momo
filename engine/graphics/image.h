#pragma once

// Image: 已解码的位图(不可变, 线程无关, 以 shared_ptr<const Image> 跨线程共享, docs/05 §6)。
// 像素在 io 线程解码(engine/assets), raster 线程首次绘制时由 Skia 按需上传 GPU 并缓存。
// 公共头不暴露 Skia 类型;Impl 定义在 graphics 内部头 skia/paint_context.h。

#include <cstddef>
#include <cstdint>
#include <memory>

#include "base/status.h"

namespace momo::engine::graphics {

class Image {
 public:
  struct Impl;

  explicit Image(std::unique_ptr<Impl> impl);
  ~Image();

  int width() const;
  int height() const;
  // 像素内存占用(用于向 V8 报告外部内存压力)。
  size_t byte_size() const;
  const Impl& impl() const { return *impl_; }

  // RGBA8888 非预乘像素(row_bytes = 0 表示紧密排列)→ Image(拷贝)。
  static StatusOr<std::shared_ptr<const Image>> FromRgba(const uint8_t* pixels, int width,
                                                         int height, size_t row_bytes = 0);

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace momo::engine::graphics
