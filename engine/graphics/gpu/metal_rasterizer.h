#pragma once

// MetalRasterizer: Metal 的 RenderBackend(docs/05 §4): Ganesh(Skia GPU), 每帧把 CAMetalDrawable 包装为
// render target。
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
#include "engine/graphics/render_backend.h"

namespace momo::engine::graphics {

class MetalRasterizer final : public RenderBackend {
 public:
  // mtl_device: id<MTLDevice>;mtl_queue: id<MTLCommandQueue>(须属于该 device)。内部各自 retain。
  static StatusOr<std::unique_ptr<MetalRasterizer>> Create(void* mtl_device, void* mtl_queue);
  ~MetalRasterizer() override;

  // RenderBackend。frame.gpu_target 是 id<CAMetalDrawable>: 画进它的纹理并提交命令缓冲(以纹理的实际尺寸为准)。
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kMetal; }
  Status Draw(LayerTree& tree, const platform::SurfaceFrame& frame) override;
  // drawable 像素 (0,0) 对应设备坐标 origin。
  Status DrawSlice(const Composition& composition, size_t slice, const platform::SurfaceFrame& frame,
                   Point origin) override;
  // 呈现在 platform 线程进行(CAMetalLayer.presentsWithTransaction): 等到目前为止提交的命令缓冲都已被 GPU 调度。
  void PrepareForExternalPresent() override;

  // 把整棵树画进 id<MTLTexture>(BGRA8Unorm, 带 RenderTarget usage)并提交。离屏/测试用。
  // clear_color: 0xAARRGGBB。
  Status DrawToTexture(LayerTree& tree, void* mtl_texture, uint32_t clear_color = 0x00000000);

 private:
  struct Impl;
  explicit MetalRasterizer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(MetalRasterizer);
};

}  // namespace momo::engine::graphics
