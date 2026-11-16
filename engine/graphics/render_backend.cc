#include "engine/graphics/render_backend.h"

#include <optional>
#include <utility>

#include "engine/graphics/software_rasterizer.h"

#if defined(__APPLE__)
#include "engine/graphics/gpu/metal_rasterizer.h"
#elif defined(_WIN32)
#include "engine/graphics/gpu/d3d_rasterizer.h"
#endif

namespace momo::engine::graphics {

namespace {

// CPU 光栅进 surface 提供的像素缓冲(无 GPU / 远程桌面回退)。
class SoftwareBackend final : public RenderBackend {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  Status Draw(LayerTree& layer_tree, const platform::SurfaceFrame& frame) override {
    MOMO_RETURN_IF_ERROR(Check(frame));
    return RasterizeLayerTree(layer_tree, frame.software);
  }
  Status DrawSlice(const Composition& composition, size_t slice, const platform::SurfaceFrame& frame,
                   Point origin) override {
    MOMO_RETURN_IF_ERROR(Check(frame));
    return composition.RasterizeSlice(slice, frame.software, origin);
  }

 private:
  static Status Check(const platform::SurfaceFrame& frame) {
    return frame.kind == platform::SurfaceKind::kSoftware
               ? Status::Ok()
               : FailedPreconditionError("SoftwareBackend: frame is not a software frame");
  }
};

// 无绘制目标(测试/占位 surface): 只走 present。
class NullBackend final : public RenderBackend {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kNone; }
  Status Draw(LayerTree&, const platform::SurfaceFrame&) override { return Status::Ok(); }
  Status DrawSlice(const Composition&, size_t, const platform::SurfaceFrame&, Point) override {
    return Status::Ok();
  }
};

}  // namespace

StatusOr<void*> RenderBackend::GpuTarget(const platform::SurfaceFrame& frame) const {
  if (frame.kind != kind() || frame.gpu_target == nullptr) {
    return FailedPreconditionError("GPU backend: the frame has no render target of this backend's kind");
  }
  return frame.gpu_target;
}

Status RenderBackend::DrawComposition(const Composition& composition,
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
  [[maybe_unused]] const platform::GpuContextInfo gpu = surface.gpu_context();
  switch (surface.kind()) {
    case platform::SurfaceKind::kSoftware:
      return std::unique_ptr<RenderBackend>(std::make_unique<SoftwareBackend>());
    case platform::SurfaceKind::kNone:
      return std::unique_ptr<RenderBackend>(std::make_unique<NullBackend>());
    case platform::SurfaceKind::kMetal: {
#if defined(__APPLE__)
      MOMO_ASSIGN_OR_RETURN(std::unique_ptr<MetalRasterizer> backend,
                            MetalRasterizer::Create(gpu.device, gpu.queue));
      return std::unique_ptr<RenderBackend>(std::move(backend));
#else
      return UnimplementedError("Metal backend is only available on Apple platforms");
#endif
    }
    case platform::SurfaceKind::kDirect3D: {
#if defined(_WIN32)
      MOMO_ASSIGN_OR_RETURN(std::unique_ptr<D3DRasterizer> backend,
                            D3DRasterizer::Create(gpu.device, gpu.queue));
      return std::unique_ptr<RenderBackend>(std::move(backend));
#else
      return UnimplementedError("Direct3D backend is only available on Windows");
#endif
    }
  }
  return UnimplementedError("No render backend for this surface kind yet");
}

}  // namespace momo::engine::graphics
