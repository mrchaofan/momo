#pragma once

// MetalRasterizer: Ganesh(Skia GPU)Metal 后端的 graphics 侧实现(docs/05 §4 MetalBackend)。
// 公共头不含 Skia/ObjC 类型(docs/10 §1): Metal 对象以 void* 传入(id<MTLDevice> / id<MTLCommandQueue> /
// id<CAMetalDrawable> / id<MTLTexture> 的 __bridge 指针, 非拥有)。
//
// 线程约定: 只在 raster 线程创建、使用与析构(持有的 GrDirectContext 归 raster 线程, docs/08 §3)。
// 析构前会同步等待已提交的 GPU 工作完成, 析构返回后不再触碰传入的 device/queue。
// 仅 Apple 平台编译(engine/CMakeLists.txt)。

#include <cstdint>
#include <memory>

#include "base/geometry.h"
#include "base/macros.h"
#include "base/status.h"
#include "engine/graphics/composition.h"
#include "engine/graphics/layer_tree.h"

namespace momo::engine::graphics {

class MetalRasterizer {
 public:
  // mtl_device: id<MTLDevice>;mtl_queue: id<MTLCommandQueue>(须属于该 device)。内部各自 retain。
  static StatusOr<std::unique_ptr<MetalRasterizer>> Create(void* mtl_device, void* mtl_queue);
  ~MetalRasterizer();

  // 把整棵树画进 drawable(id<CAMetalDrawable>)的纹理并提交命令缓冲(不 present)。
  // size: 帧的物理像素尺寸(应与 drawable 纹理一致;以纹理实际尺寸为准)。
  // clear_color: 0xAARRGGBB(默认全透明, 与软件后端一致)。
  Status Draw(LayerTree& tree, void* drawable, Size size, uint32_t clear_color = 0x00000000);
  // 同上, 目标为 id<MTLTexture>(BGRA8Unorm, 带 RenderTarget usage)。离屏/测试用。
  Status DrawToTexture(LayerTree& tree, void* mtl_texture, Size size,
                       uint32_t clear_color = 0x00000000);
  // 平台视图合成(docs/05 §9): 把分片 slice 画进 drawable(id<CAMetalDrawable>)并提交(不 present)。
  // drawable 像素 (0,0) 对应设备坐标 origin。
  Status DrawSlice(const Composition& composition, size_t slice, void* drawable, Point origin,
                   uint32_t clear_color = 0x00000000);
  // 等待到目前为止提交的命令缓冲都已被 GPU 调度(之后可在其他线程 present drawable,
  // CAMetalLayer.presentsWithTransaction 要求)。
  void WaitUntilScheduled();
  // 阻塞等待已提交的 GPU 工作完成(测试回读、关闭前使用)。
  void WaitForGpu();

 private:
  struct Impl;
  explicit MetalRasterizer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(MetalRasterizer);
};

}  // namespace momo::engine::graphics
