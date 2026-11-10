#include "engine/graphics/gpu/metal_rasterizer.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <utility>

#include "engine/graphics/skia/composition_impl.h"
#include "engine/graphics/skia/paint_context.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"

namespace momo::engine::graphics {

struct MetalRasterizer::Impl {
  ~Impl() {
    if (context != nullptr) {
      // GPU 资源归 raster 线程: 在这里(raster 线程)等待在途工作完成后释放。
      context->flushAndSubmit(GrSyncCpu::kYes);
      context.reset();
    }
  }

  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  sk_sp<GrDirectContext> context;
};

MetalRasterizer::MetalRasterizer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MetalRasterizer::~MetalRasterizer() = default;

StatusOr<std::unique_ptr<MetalRasterizer>> MetalRasterizer::Create(void* mtl_device,
                                                                   void* mtl_queue) {
  if (mtl_device == nullptr || mtl_queue == nullptr) {
    return InvalidArgumentError("MetalRasterizer: missing MTLDevice/MTLCommandQueue");
  }
  auto impl = std::make_unique<Impl>();
  impl->device = (__bridge id<MTLDevice>)mtl_device;
  impl->queue = (__bridge id<MTLCommandQueue>)mtl_queue;
  if (impl->queue.device != impl->device) {
    return InvalidArgumentError("MetalRasterizer: command queue belongs to another device");
  }
  @autoreleasepool {
    GrMtlBackendContext backend;
    backend.fDevice.retain((__bridge GrMTLHandle)impl->device);
    backend.fQueue.retain((__bridge GrMTLHandle)impl->queue);
    GrContextOptions options;
    impl->context = GrDirectContexts::MakeMetal(backend, options);
  }
  if (impl->context == nullptr) {
    return UnavailableError("GrDirectContexts::MakeMetal failed");
  }
  return std::unique_ptr<MetalRasterizer>(new MetalRasterizer(std::move(impl)));
}

Status MetalRasterizer::Draw(LayerTree& tree, void* drawable, Size size, uint32_t clear_color) {
  if (drawable == nullptr) {
    return InvalidArgumentError("MetalRasterizer: null drawable");
  }
  id<CAMetalDrawable> metal_drawable = (__bridge id<CAMetalDrawable>)drawable;
  return DrawToTexture(tree, (__bridge void*)metal_drawable.texture, size, clear_color);
}

namespace {

// 把 id<MTLTexture> 包装为 SkSurface, 在其画布上执行 draw 并提交到 queue(不 present)。
template <typename DrawFn>
Status DrawIntoTexture(GrDirectContext* context, void* mtl_texture, DrawFn&& draw) {
  if (mtl_texture == nullptr) {
    return InvalidArgumentError("MetalRasterizer: null texture");
  }
  if (context->abandoned()) {
    return UnavailableError("MetalRasterizer: GPU context lost");
  }
  @autoreleasepool {
    id<MTLTexture> texture = (__bridge id<MTLTexture>)mtl_texture;
    // 以纹理实际尺寸为准(调用方给出的 size 仅作期望;二者不一致时内容按左上角对齐裁剪)。
    const int width = static_cast<int>(texture.width);
    const int height = static_cast<int>(texture.height);
    if (width <= 0 || height <= 0) {
      return InvalidArgumentError("MetalRasterizer: empty texture");
    }
    GrMtlTextureInfo info;
    info.fTexture.retain((__bridge GrMTLHandle)texture);
    GrBackendRenderTarget target = GrBackendRenderTargets::MakeMtl(width, height, info);
    // 与软件后端一致: 不带色彩空间(像素按 sRGB 解释, 由 CAMetalLayer.colorspace 交给 WindowServer 匹配)。
    sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(
        context, target, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, nullptr, nullptr);
    if (surface == nullptr) {
      return InternalError("MetalRasterizer: cannot wrap render target");
    }
    draw(surface->getCanvas());
    // 提交到 queue;surface 的 present 在同一 queue 上排在其后。
    context->flushAndSubmit(surface.get(), GrSyncCpu::kNo);
  }
  return Status::Ok();
}

}  // namespace

Status MetalRasterizer::DrawToTexture(LayerTree& tree, void* mtl_texture, Size size,
                                      uint32_t clear_color) {
  (void)size;
  return DrawIntoTexture(impl_->context.get(), mtl_texture, [&](SkCanvas* canvas) {
    canvas->clear(static_cast<SkColor>(clear_color));
    if (tree.root() != nullptr) {
      PrerollContext preroll;
      tree.root()->Preroll(preroll);
      PaintContext paint{canvas};
      tree.root()->Paint(paint);
    }
  });
}

Status MetalRasterizer::DrawSlice(const Composition& composition, size_t slice, void* drawable,
                                  Point origin, uint32_t clear_color) {
  if (drawable == nullptr) {
    return InvalidArgumentError("MetalRasterizer: null drawable");
  }
  id<CAMetalDrawable> metal_drawable = (__bridge id<CAMetalDrawable>)drawable;
  return DrawIntoTexture(impl_->context.get(), (__bridge void*)metal_drawable.texture,
                         [&](SkCanvas* canvas) {
                           DrawCompositionSlice(composition, slice, canvas, origin, clear_color);
                         });
}

void MetalRasterizer::WaitUntilScheduled() {
  @autoreleasepool {
    // 同一 queue 上的命令缓冲按提交顺序调度: 空命令缓冲被调度时, 之前提交的都已调度。
    id<MTLCommandBuffer> marker = [impl_->queue commandBuffer];
    if (marker == nil) {
      return;
    }
    [marker commit];
    [marker waitUntilScheduled];
  }
}

void MetalRasterizer::WaitForGpu() {
  @autoreleasepool {
    impl_->context->flushAndSubmit(GrSyncCpu::kYes);
  }
}

}  // namespace momo::engine::graphics
