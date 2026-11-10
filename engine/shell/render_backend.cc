#include "engine/shell/render_backend.h"

#include <optional>
#include <utility>

#include "engine/graphics/software_rasterizer.h"

#if defined(__APPLE__)
#include "engine/graphics/gpu/metal_rasterizer.h"
#endif

namespace momo::engine {

namespace {

// CPU 光栅进 surface 提供的像素缓冲(无 GPU / 远程桌面回退)。
class SoftwareBackend final : public RenderBackend {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  Status Draw(graphics::LayerTree& layer_tree, const platform::SurfaceFrame& frame) override {
    if (frame.kind != platform::SurfaceKind::kSoftware) {
      return FailedPreconditionError("SoftwareBackend: frame is not a software frame");
    }
    const platform::SoftwarePixels& px = frame.software;
    return graphics::RasterizeLayerTree(layer_tree, {px.pixels, px.row_bytes, px.width, px.height});
  }
  Status DrawComposition(const graphics::Composition& composition,
                         const platform::CompositionFrame& frame) override {
    MOMO_RETURN_IF_ERROR(DrawSlice(composition, 0, frame.base, {0, 0}));
    for (size_t i = 0; i < frame.overlays.size(); ++i) {
      const std::optional<Rect> bounds = composition.slice_bounds(i + 1);
      if (frame.overlays[i] && bounds) {
        MOMO_RETURN_IF_ERROR(DrawSlice(composition, i + 1, *frame.overlays[i], {bounds->x, bounds->y}));
      }
    }
    return Status::Ok();
  }

 private:
  static Status DrawSlice(const graphics::Composition& composition, size_t slice,
                          const platform::SurfaceFrame& frame, Point origin) {
    if (frame.kind != platform::SurfaceKind::kSoftware) {
      return FailedPreconditionError("SoftwareBackend: frame is not a software frame");
    }
    const platform::SoftwarePixels& px = frame.software;
    return composition.RasterizeSlice(slice, {px.pixels, px.row_bytes, px.width, px.height}, origin);
  }
};

// 无绘制目标(测试/占位 surface): 只走 present。
class NullBackend final : public RenderBackend {
 public:
  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kNone; }
  Status Draw(graphics::LayerTree&, const platform::SurfaceFrame&) override { return Status::Ok(); }
  Status DrawComposition(const graphics::Composition&, const platform::CompositionFrame&) override {
    return Status::Ok();
  }
};

#if defined(__APPLE__)
// Ganesh Metal: 每帧把 CAMetalDrawable 包装为 render target(graphics::MetalRasterizer)。
class MetalBackend final : public RenderBackend {
 public:
  explicit MetalBackend(std::unique_ptr<graphics::MetalRasterizer> rasterizer)
      : rasterizer_(std::move(rasterizer)) {}

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kMetal; }
  Status Draw(graphics::LayerTree& layer_tree, const platform::SurfaceFrame& frame) override {
    if (frame.kind != platform::SurfaceKind::kMetal || frame.gpu_target == nullptr) {
      return FailedPreconditionError("MetalBackend: frame has no Metal drawable");
    }
    return rasterizer_->Draw(layer_tree, frame.gpu_target, frame.size);
  }
  Status DrawComposition(const graphics::Composition& composition,
                         const platform::CompositionFrame& frame) override {
    if (frame.base.kind != platform::SurfaceKind::kMetal || frame.base.gpu_target == nullptr) {
      return FailedPreconditionError("MetalBackend: composition frame has no Metal drawable");
    }
    MOMO_RETURN_IF_ERROR(rasterizer_->DrawSlice(composition, 0, frame.base.gpu_target, {0, 0}));
    for (size_t i = 0; i < frame.overlays.size(); ++i) {
      const std::optional<Rect> bounds = composition.slice_bounds(i + 1);
      if (!frame.overlays[i] || !bounds || frame.overlays[i]->gpu_target == nullptr) {
        continue;
      }
      MOMO_RETURN_IF_ERROR(rasterizer_->DrawSlice(composition, i + 1, frame.overlays[i]->gpu_target,
                                                  {bounds->x, bounds->y}));
    }
    // 呈现在 platform 线程进行(presentsWithTransaction): 先确保命令已被调度。
    rasterizer_->WaitUntilScheduled();
    return Status::Ok();
  }
  void PrepareForExternalPresent() override { rasterizer_->WaitUntilScheduled(); }

 private:
  std::unique_ptr<graphics::MetalRasterizer> rasterizer_;  // 持有 GrDirectContext(raster 线程)
};
#endif

}  // namespace

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
    case platform::SurfaceKind::kAngleGl:
      break;  // TODO(M2, Windows): AngleGlBackend
  }
  return UnimplementedError("No render backend for this surface kind yet");
}

}  // namespace momo::engine
