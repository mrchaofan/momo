#include "engine/shell/render_backend.h"

#include <optional>
#include <utility>

#include "engine/graphics/software_rasterizer.h"

#if defined(__APPLE__)
#include "engine/graphics/gpu/metal_rasterizer.h"
#elif defined(_WIN32)
#include "engine/graphics/gpu/d3d_rasterizer.h"
#endif

namespace momo::engine {

namespace {

// CPU 光栅进 surface 提供的像素缓冲(无 GPU / 远程桌面回退)。
class SoftwareBackend final : public RenderBackend {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  Status Draw(graphics::LayerTree& layer_tree, const platform::SurfaceFrame& frame) override {
    MOMO_ASSIGN_OR_RETURN(graphics::PixelBuffer pixels, Pixels(frame));
    return graphics::RasterizeLayerTree(layer_tree, pixels);
  }
  Status DrawSlice(const graphics::Composition& composition, size_t slice,
                   const platform::SurfaceFrame& frame, Point origin) override {
    MOMO_ASSIGN_OR_RETURN(graphics::PixelBuffer pixels, Pixels(frame));
    return composition.RasterizeSlice(slice, pixels, origin);
  }

 private:
  static StatusOr<graphics::PixelBuffer> Pixels(const platform::SurfaceFrame& frame) {
    if (frame.kind != platform::SurfaceKind::kSoftware) {
      return FailedPreconditionError("SoftwareBackend: frame is not a software frame");
    }
    const platform::SoftwarePixels& px = frame.software;
    return graphics::PixelBuffer{px.pixels, px.row_bytes, px.width, px.height};
  }
};

// 无绘制目标(测试/占位 surface): 只走 present。
class NullBackend final : public RenderBackend {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kNone; }
  Status Draw(graphics::LayerTree&, const platform::SurfaceFrame&) override { return Status::Ok(); }
  Status DrawSlice(const graphics::Composition&, size_t, const platform::SurfaceFrame&, Point) override {
    return Status::Ok();
  }
};

// GPU 帧的绘制目标(Metal: id<CAMetalDrawable>;Direct3D: 后台缓冲 ID3D12Resource*)。
[[maybe_unused]] StatusOr<void*> GpuTarget(const platform::SurfaceFrame& frame, platform::SurfaceKind kind) {
  if (frame.kind != kind || frame.gpu_target == nullptr) {
    return FailedPreconditionError("GPU backend: the frame has no render target of this backend's kind");
  }
  return frame.gpu_target;
}

#if defined(__APPLE__)
// Ganesh Metal: 每帧把 CAMetalDrawable 包装为 render target(graphics::MetalRasterizer)。
class MetalBackend final : public RenderBackend {
 public:
  explicit MetalBackend(std::unique_ptr<graphics::MetalRasterizer> rasterizer)
      : rasterizer_(std::move(rasterizer)) {}

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kMetal; }
  Status Draw(graphics::LayerTree& layer_tree, const platform::SurfaceFrame& frame) override {
    MOMO_ASSIGN_OR_RETURN(void* drawable, GpuTarget(frame, kind()));
    return rasterizer_->Draw(layer_tree, drawable, frame.size);
  }
  Status DrawSlice(const graphics::Composition& composition, size_t slice,
                   const platform::SurfaceFrame& frame, Point origin) override {
    MOMO_ASSIGN_OR_RETURN(void* drawable, GpuTarget(frame, kind()));
    return rasterizer_->DrawSlice(composition, slice, drawable, origin);
  }
  // 呈现在 platform 线程进行(presentsWithTransaction): 先确保命令已被调度。
  void PrepareForExternalPresent() override { rasterizer_->WaitUntilScheduled(); }

 private:
  std::unique_ptr<graphics::MetalRasterizer> rasterizer_;  // 持有 GrDirectContext(raster 线程)
};
#endif

#if defined(_WIN32)
// Ganesh Direct3D 12: 每帧把 swapchain 的后台缓冲包装为 render target(graphics::D3DRasterizer)。
// 画完即已转回 PRESENT 状态并等待 GPU 完成, surface 的 present 可以直接 Present。
class Direct3DBackend final : public RenderBackend {
 public:
  explicit Direct3DBackend(std::unique_ptr<graphics::D3DRasterizer> rasterizer)
      : rasterizer_(std::move(rasterizer)) {}

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kDirect3D; }
  Status Draw(graphics::LayerTree& layer_tree, const platform::SurfaceFrame& frame) override {
    MOMO_ASSIGN_OR_RETURN(void* back_buffer, GpuTarget(frame, kind()));
    return rasterizer_->Draw(layer_tree, back_buffer, frame.size);
  }
  Status DrawSlice(const graphics::Composition& composition, size_t slice,
                   const platform::SurfaceFrame& frame, Point origin) override {
    MOMO_ASSIGN_OR_RETURN(void* back_buffer, GpuTarget(frame, kind()));
    return rasterizer_->DrawSlice(composition, slice, back_buffer, origin);
  }

 private:
  std::unique_ptr<graphics::D3DRasterizer> rasterizer_;  // 持有 GrDirectContext(raster 线程)
};
#endif

}  // namespace

Status RenderBackend::DrawComposition(const graphics::Composition& composition,
                                      const platform::CompositionFrame& frame) {
  MOMO_RETURN_IF_ERROR(DrawSlice(composition, 0, frame.base, {0, 0}));
  for (size_t i = 0; i < frame.overlays.size(); ++i) {
    const std::optional<Rect> bounds = composition.slice_bounds(i + 1);
    if (frame.overlays[i] && bounds) {
      MOMO_RETURN_IF_ERROR(DrawSlice(composition, i + 1, *frame.overlays[i], {bounds->x, bounds->y}));
    }
  }
  PrepareForExternalPresent();
  return Status::Ok();
}

StatusOr<std::unique_ptr<RenderBackend>> CreateRenderBackend(platform::RenderSurface& surface) {
  switch (surface.kind()) {
    case platform::SurfaceKind::kSoftware:
      return std::unique_ptr<RenderBackend>(std::make_unique<SoftwareBackend>());
    case platform::SurfaceKind::kNone:
      return std::unique_ptr<RenderBackend>(std::make_unique<NullBackend>());
    case platform::SurfaceKind::kMetal: {
#if defined(__APPLE__)
      const platform::GpuContextInfo gpu = surface.gpu_context();
      MOMO_ASSIGN_OR_RETURN(std::unique_ptr<graphics::MetalRasterizer> rasterizer,
                            graphics::MetalRasterizer::Create(gpu.device, gpu.queue));
      return std::unique_ptr<RenderBackend>(std::make_unique<MetalBackend>(std::move(rasterizer)));
#else
      return UnimplementedError("Metal backend is only available on Apple platforms");
#endif
    }
    case platform::SurfaceKind::kDirect3D: {
#if defined(_WIN32)
      const platform::GpuContextInfo gpu = surface.gpu_context();
      MOMO_ASSIGN_OR_RETURN(std::unique_ptr<graphics::D3DRasterizer> rasterizer,
                            graphics::D3DRasterizer::Create(gpu.device, gpu.queue));
      return std::unique_ptr<RenderBackend>(std::make_unique<Direct3DBackend>(std::move(rasterizer)));
#else
      return UnimplementedError("Direct3D backend is only available on Windows");
#endif
    }
  }
  return UnimplementedError("No render backend for this surface kind yet");
}

}  // namespace momo::engine
