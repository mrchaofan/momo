#pragma once

// RenderBackend: 把 LayerTree 画进一帧 SurfaceFrame 的后端(docs/05 §4)。Rasterizer 按 surface 种类
// 经 CreateRenderBackend 选取;新增后端只需新增实现并在工厂登记, Rasterizer 不变。
//   kSoftware → SoftwareBackend(CPU 光栅进 SoftwarePixels)
//   kMetal    → MetalBackend(Apple;GrDirectContext 来自 surface.gpu_context())
//   kDirect3D → Direct3DBackend(Windows;Ganesh D3D12, 设备与队列来自 surface.gpu_context())
//   kNone     → NullBackend(只走 present, 测试/占位)
// GPU 后端创建失败时由 Rasterizer 负责回退(surface.FallBackToSoftware() 后重建, rasterizer.cc)。
// 线程约定: raster 线程创建、使用与析构(GPU 资源归 raster 线程, docs/08 §3)。

#include <memory>

#include "base/status.h"
#include "engine/graphics/composition.h"
#include "engine/graphics/layer_tree.h"
#include "engine/platform/render_surface.h"

namespace momo::engine {

class RenderBackend {
 public:
  virtual ~RenderBackend() = default;
  // 后端对应的 surface 种类。
  virtual platform::SurfaceKind kind() const = 0;
  // 绘制(不 present)。GPU 后端在返回前提交命令缓冲。
  virtual Status Draw(graphics::LayerTree& layer_tree, const platform::SurfaceFrame& frame) = 0;
  // 平台视图合成(docs/05 §9): 分片 0 画进 frame.base, 分片 i+1 画进 frame.overlays[i](为空则跳过)。
  // 不 present;返回前保证命令已提交且可以在其他线程呈现。
  virtual Status DrawComposition(const graphics::Composition& composition,
                                 const platform::CompositionFrame& frame) = 0;
  // Draw 之后由别处(而非 SurfaceFrame::present)呈现时调用: 保证命令已可被其他线程呈现。
  virtual void PrepareForExternalPresent() {}
};

// 按 surface.kind() 创建后端;GPU 后端从 surface.gpu_context() 取设备。
// 不支持的种类返回 Unimplemented, GPU 初始化失败返回对应错误。
StatusOr<std::unique_ptr<RenderBackend>> CreateRenderBackend(platform::RenderSurface& surface);

}  // namespace momo::engine
