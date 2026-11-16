#pragma once

// 软件光栅化(docs/05 §4 SoftwareBackend 的 M1 形态): 把 LayerTree 画进调用方提供的像素缓冲。
// 用于: 无 GPU / 远程桌面回退、headless 测试与 golden 比对。
// 线程约定: raster 线程(或测试线程);函数本身无共享状态。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/status.h"
#include "engine/graphics/layer_tree.h"
#include "engine/platform/render_surface.h"

namespace momo::engine::graphics {

// 像素缓冲就是平台契约里的软件帧: 32bpp 预乘 alpha, 字节序 B,G,R,A。
using PixelBuffer = platform::SoftwarePixels;

// 先清为 clear_color(0xAARRGGBB, 默认全透明), 再 Preroll + Paint 整棵树。
Status RasterizeLayerTree(LayerTree& tree, const PixelBuffer& target,
                          uint32_t clear_color = 0x00000000);

// 拥有像素内存的缓冲(测试与离屏渲染用)。
class OwnedPixelBuffer {
 public:
  OwnedPixelBuffer(int width, int height)
      : width_(width), height_(height), data_(static_cast<size_t>(width) * height, 0) {}
  PixelBuffer view() { return {data_.data(), static_cast<size_t>(width_) * 4, width_, height_}; }
  int width() const { return width_; }
  int height() const { return height_; }
  // 读取 (x, y) 处像素, 返回反预乘的 0xAARRGGBB。
  uint32_t GetColor(int x, int y) const;
  const std::vector<uint32_t>& data() const { return data_; }

 private:
  int width_;
  int height_;
  std::vector<uint32_t> data_;
};

// PNG 编解码(golden 测试用)。像素为 OwnedPixelBuffer 的格式。
StatusOr<std::vector<uint8_t>> EncodePng(const PixelBuffer& pixels);
Status DecodePng(const std::vector<uint8_t>& png, OwnedPixelBuffer* out);

}  // namespace momo::engine::graphics
