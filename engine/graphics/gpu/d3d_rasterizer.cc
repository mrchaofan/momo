#include "engine/graphics/gpu/d3d_rasterizer.h"

// GrD3DTypes.h 会包含 <d3d12.h> / <windows.h>: 只能出现在本实现文件里。
#include <dxgi1_4.h>

#include <utility>

#include "engine/graphics/skia/composition_impl.h"
#include "engine/graphics/skia/paint_context.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"

namespace momo::engine::graphics {

struct D3DRasterizer::Impl {
  ~Impl() {
    if (context != nullptr) {
      if (device->GetDeviceRemovedReason() != S_OK) {
        // 设备已丢失(surface 回退软件后重建后端时): 不能再等待 GPU, 放弃全部 GPU 对象。
        context->abandonContext();
      } else {
        // GPU 资源归 raster 线程: 在这里(raster 线程)等待在途工作完成后释放。
        context->flushAndSubmit(GrSyncCpu::kYes);
      }
      context.reset();
    }
  }

  gr_cp<ID3D12Device> device;
  gr_cp<ID3D12CommandQueue> queue;
  sk_sp<GrDirectContext> context;
};

D3DRasterizer::D3DRasterizer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

D3DRasterizer::~D3DRasterizer() = default;

StatusOr<std::unique_ptr<D3DRasterizer>> D3DRasterizer::Create(void* device, void* queue) {
  if (device == nullptr || queue == nullptr) {
    return InvalidArgumentError("D3DRasterizer: missing ID3D12Device/ID3D12CommandQueue");
  }
  auto impl = std::make_unique<Impl>();
  impl->device.retain(static_cast<ID3D12Device*>(device));
  impl->queue.retain(static_cast<ID3D12CommandQueue*>(queue));

  // GrD3DBackendContext 还需要设备所属的适配器: 按 LUID 找回。
  gr_cp<IDXGIFactory4> factory;
  if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return UnavailableError("D3DRasterizer: CreateDXGIFactory1 failed");
  }
  gr_cp<IDXGIAdapter1> adapter;
  const LUID luid = impl->device->GetAdapterLuid();
  if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
    return UnavailableError("D3DRasterizer: adapter of the device not found");
  }
  GrD3DBackendContext backend;
  backend.fAdapter = std::move(adapter);
  backend.fDevice = impl->device;
  backend.fQueue = impl->queue;
  GrContextOptions options;
  impl->context = GrDirectContext::MakeDirect3D(backend, options);
  if (impl->context == nullptr) {
    return UnavailableError("GrDirectContext::MakeDirect3D failed");
  }
  return std::unique_ptr<D3DRasterizer>(new D3DRasterizer(std::move(impl)));
}

namespace {

// 把后台缓冲包装为 SkSurface, 在其画布上执行 draw, 转回 PRESENT 状态并提交, 等待 GPU 完成(不 present)。
template <typename DrawFn>
Status DrawIntoResource(GrDirectContext* context, void* resource, DrawFn&& draw) {
  if (resource == nullptr) {
    return InvalidArgumentError("D3DRasterizer: null resource");
  }
  if (context->abandoned()) {
    return UnavailableError("D3DRasterizer: GPU context lost");
  }
  auto* d3d_resource = static_cast<ID3D12Resource*>(resource);
  const D3D12_RESOURCE_DESC desc = d3d_resource->GetDesc();
  // 以资源实际尺寸为准(调用方给出的 size 仅作期望)。
  const int width = static_cast<int>(desc.Width);
  const int height = static_cast<int>(desc.Height);
  if (width <= 0 || height <= 0) {
    return InvalidArgumentError("D3DRasterizer: empty resource");
  }
  SkColorType color_type = kBGRA_8888_SkColorType;
  if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
    color_type = kRGBA_8888_SkColorType;
  } else if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
    return InvalidArgumentError("D3DRasterizer: unsupported back buffer format");
  }
  GrD3DTextureResourceInfo info;
  info.fResource.retain(d3d_resource);
  info.fResourceState = D3D12_RESOURCE_STATE_PRESENT;  // swapchain 后台缓冲在 present 之间的状态
  info.fFormat = desc.Format;
  info.fSampleCount = 1;
  info.fLevelCount = 1;
  GrBackendRenderTarget target(width, height, info);
  {
    // 与软件后端一致: 不带色彩空间(像素按 sRGB 解释)。
    sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(context, target, kTopLeft_GrSurfaceOrigin,
                                                                   color_type, nullptr, nullptr);
    if (surface == nullptr) {
      return InternalError("D3DRasterizer: cannot wrap render target");
    }
    draw(surface->getCanvas());
    // 转回 PRESENT 状态并提交;等待完成, 让 Skia 释放对后台缓冲的引用(ResizeBuffers 的前提, 见头文件)。
    context->flush(surface.get(), SkSurfaces::BackendSurfaceAccess::kPresent, GrFlushInfo{});
    context->submit(GrSyncCpu::kYes);
  }
  return Status::Ok();
}

}  // namespace

Status D3DRasterizer::Draw(LayerTree& tree, void* resource, Size size, uint32_t clear_color) {
  (void)size;
  return DrawIntoResource(impl_->context.get(), resource, [&](SkCanvas* canvas) {
    canvas->clear(static_cast<SkColor>(clear_color));
    if (tree.root() != nullptr) {
      PrerollContext preroll;
      tree.root()->Preroll(preroll);
      PaintContext paint{canvas};
      tree.root()->Paint(paint);
    }
  });
}

Status D3DRasterizer::DrawSlice(const Composition& composition, size_t slice, void* resource, Point origin,
                                uint32_t clear_color) {
  return DrawIntoResource(impl_->context.get(), resource, [&](SkCanvas* canvas) {
    DrawCompositionSlice(composition, slice, canvas, origin, clear_color);
  });
}

void D3DRasterizer::WaitForGpu() { impl_->context->flushAndSubmit(GrSyncCpu::kYes); }

}  // namespace momo::engine::graphics
