#include "embedder/common/fallback_surface.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "base/logging.h"

namespace momo::embedder {

using engine::platform::CompositionFrame;
using engine::platform::CompositionLayout;
using engine::platform::GpuContextInfo;
using engine::platform::SurfaceFrame;
using engine::platform::SurfaceKind;

bool SoftwareRenderingForced() {
  const char* value = std::getenv("MOMO_SOFTWARE_RENDERING");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

FallbackSurface::FallbackSurface(std::unique_ptr<GpuSurface> gpu,
                                 std::unique_ptr<engine::platform::RenderSurface> software)
    : gpu_(std::move(gpu)), software_(std::move(software)) {}

FallbackSurface::~FallbackSurface() = default;

SurfaceKind FallbackSurface::kind() const {
  return software_active() ? software_->kind() : gpu_->kind();
}

GpuContextInfo FallbackSurface::gpu_context() const {
  return software_active() ? GpuContextInfo{} : gpu_->gpu_context();
}

bool FallbackSurface::FallBackToSoftware() {
  if (!software_active_.exchange(true, std::memory_order_acq_rel)) {
    MOMO_LOG(WARNING) << "The GPU surface is unavailable; falling back to software presentation.";
    gpu_->Deactivate();
  }
  return true;
}

bool FallbackSurface::UseSoftware() {
  if (!software_active() && gpu_->device_lost()) {
    MOMO_LOG(ERROR) << "The GPU device was lost.";
    FallBackToSoftware();
  }
  return software_active();
}

StatusOr<SurfaceFrame> FallbackSurface::AcquireFrame(Size physical_size) {
  return UseSoftware() ? software_->AcquireFrame(physical_size) : gpu_->AcquireFrame(physical_size);
}

bool FallbackSurface::SupportsPlatformViews() const {
  return software_active() ? software_->SupportsPlatformViews() : gpu_->SupportsPlatformViews();
}

StatusOr<CompositionFrame> FallbackSurface::AcquireCompositionFrame(Size physical_size,
                                                                   const CompositionLayout& layout) {
  return UseSoftware() ? software_->AcquireCompositionFrame(physical_size, layout)
                       : gpu_->AcquireCompositionFrame(physical_size, layout);
}

}  // namespace momo::embedder
