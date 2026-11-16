#pragma once

// RenderBackend: 把 LayerTree 画进一帧 SurfaceFrame 的后端(ARCHITECTURE.md §4.4)。shell 的 Rasterizer 按 surface 种类
// 经 CreateRenderBackend 选取;新增后端只需新增实现并在工厂登记, Rasterizer 不变。
//   kSoftware → 软件光栅(CPU 画进 SoftwarePixels)
//   kMetal    → MetalRasterizer(Apple;GrDirectContext 建在 surface.gpu_context() 给出的设备与队列上)
//   kDirect3D → D3DRasterizer(Windows;Ganesh D3D12, 同上)
//   kNone     → 空后端(只走 present, 测试/占位)
// 一个后端不属于某个视图: 它只持有设备级的状态(GPU 后端是一份 GrDirectContext 及其资源缓存), 可以依次
// 画进任意多个使用同一设备的 surface。Rasterizer 据此让同一设备上的全部视图共用一个后端。
// GPU 后端创建失败时由 Rasterizer 负责回退(surface.FallBackToSoftware() 后重建, rasterizer.cc)。
// 线程约定: raster 线程创建、使用与析构(GPU 资源归 raster 线程, ARCHITECTURE.md §2.1)。

#include <memory>

#include "base/status.h"
#include "engine/graphics/composition.h"
#include "engine/graphics/layer_tree.h"
#include "engine/platform/render_surface.h"

namespace momo::engine::graphics {

class RenderBackend {
 public:
  virtual ~RenderBackend() = default;
  // 后端对应的 surface 种类。
  virtual platform::SurfaceKind kind() const = 0;
  // 绘制(不 present)。GPU 后端在返回前提交命令缓冲。
  virtual Status Draw(LayerTree& layer_tree, const platform::SurfaceFrame& frame) = 0;
  // 把合成的第 slice 个分片画进 frame(帧内像素 (0,0) 对应设备坐标 origin)。不 present。
  virtual Status DrawSlice(const Composition& composition, size_t slice,
                           const platform::SurfaceFrame& frame, Point origin) = 0;
  // 画完的帧由别处(而非 SurfaceFrame::present)呈现之前调用: 保证命令已可被其他线程呈现。
  virtual void PrepareForExternalPresent() {}

  // 平台视图合成(ARCHITECTURE.md §4.10): 分片 0 画进 frame.base, 分片 i+1 画进 frame.overlays[i](为空则跳过),
  // 然后 PrepareForExternalPresent(合成帧总是由 CompositionFrame::present 在别处呈现)。不 present。
  Status DrawComposition(const Composition& composition,
                         const platform::CompositionFrame& frame);

 protected:
  // GPU 帧的绘制目标(Metal: id<CAMetalDrawable>;Direct3D: 后台缓冲 ID3D12Resource*);帧不属于本后端时报错。
  StatusOr<void*> GpuTarget(const platform::SurfaceFrame& frame) const;
};

// 按 surface.kind() 创建后端;GPU 后端从 surface.gpu_context() 取设备。
// 不支持的种类返回 Unimplemented, GPU 初始化失败返回对应错误。
StatusOr<std::unique_ptr<RenderBackend>> CreateRenderBackend(platform::RenderSurface& surface);

}  // namespace momo::engine::graphics
