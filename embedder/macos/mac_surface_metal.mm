#include "embedder/macos/mac_surface_metal.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <optional>

#include "base/logging.h"
#include "embedder/macos/mac_platform_views.h"

// 主帧 CAMetalLayer 的宿主视图: 纯显示, 不参与命中测试。
@interface MomoSurfaceBaseView : NSView
@end
@implementation MomoSurfaceBaseView
- (NSView*)hitTest:(NSPoint)point {
  return nil;
}
@end

namespace momo::embedder::macos {

StatusOr<std::shared_ptr<const MacMetalDevice>> MacMetalDevice::Create() {
  auto shared = std::make_shared<MacMetalDevice>();
  shared->device = MTLCreateSystemDefaultDevice();
  if (shared->device == nil) {
    return UnavailableError("No Metal device available");
  }
  shared->queue = [shared->device newCommandQueue];
  if (shared->queue == nil) {
    return UnavailableError("Cannot create MTLCommandQueue");
  }
  return std::shared_ptr<const MacMetalDevice>(std::move(shared));
}

MacMetalSurface::MacMetalSurface(NSView* view, std::shared_ptr<const MacMetalDevice> device,
                                 std::shared_ptr<PresentQueue> presents)
    : view_(view), device_(std::move(device)), presents_(std::move(presents)) {
  layer_ = MakeLayer();
  // 主帧放在内容视图最底层的子视图里: 之后加入的平台视图宿主(MomoPlatformViews)按子视图顺序位于其上。
  base_view_ = [[MomoSurfaceBaseView alloc] initWithFrame:view.bounds];
  base_view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  base_view_.wantsLayer = YES;
  [view addSubview:base_view_ positioned:NSWindowBelow relativeTo:nil];
  // resize 期间新帧到来前先拉伸旧内容(与软件路径一致)。
  layer_.contentsGravity = kCAGravityResize;
  layer_.needsDisplayOnBoundsChange = NO;
  [base_view_.layer addSublayer:layer_];
  UpdateGeometry();
  layer_.drawableSize = [view convertSizeToBacking:view.bounds.size];
}

CAMetalLayer* MacMetalSurface::MakeLayer() const {
  CAMetalLayer* layer = [CAMetalLayer layer];
  layer.device = device_->device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = NO;
  layer.opaque = NO;  // 与软件路径一致: 透明像素透出视图底色(闪屏色)
  layer.maximumDrawableCount = 3;
  layer.allowsNextDrawableTimeout = YES;
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  layer.colorspace = srgb;
  CGColorSpaceRelease(srgb);
  return layer;
}

MacMetalSurface::~MacMetalSurface() {
  // platform 线程: raster 已不再使用本 surface(Shell 解绑之后才销毁窗口)。
  [layer_ removeFromSuperlayer];
  layer_ = nil;
  [base_view_ removeFromSuperview];
  base_view_ = nil;
}

platform::GpuContextInfo MacMetalSurface::gpu_context() const {
  return {(__bridge void*)device_->device, (__bridge void*)device_->queue};
}

void MacMetalSurface::Deactivate() {
  CAMetalLayer* __weak weak_layer = layer_;
  MomoPlatformViews* __weak weak_views = platform_views_;
  dispatch_async(dispatch_get_main_queue(), ^{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [weak_layer removeFromSuperlayer];  // 视图自己的 layer(软件帧的 contents)重新可见
    [weak_views hideAll];               // 软件路径不支持平台视图
    [CATransaction commit];
  });
}

StatusOr<id<CAMetalDrawable>> MacMetalSurface::NextDrawable(CAMetalLayer* layer,
                                                             Size physical_size) {
  const CGSize size = CGSizeMake(static_cast<CGFloat>(static_cast<int>(physical_size.width)),
                                 static_cast<CGFloat>(static_cast<int>(physical_size.height)));
  if (size.width <= 0 || size.height <= 0) {
    return FailedPreconditionError("empty surface size");
  }
  if (!CGSizeEqualToSize(layer.drawableSize, size)) {
    // raster 线程没有 run loop: 显式事务, 立即提交。
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.drawableSize = size;
    [CATransaction commit];
  }
  id<CAMetalDrawable> drawable = [layer nextDrawable];
  if (drawable == nil) {
    return UnavailableError("CAMetalLayer nextDrawable returned nil; frame dropped");
  }
  return drawable;
}

void MacMetalSurface::SetPresentsWithTransaction(bool value) {
  if (presents_with_transaction_ == value) {
    return;
  }
  presents_with_transaction_ = value;
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  layer_.presentsWithTransaction = value;
  [CATransaction commit];
}

StatusOr<platform::SurfaceFrame> MacMetalSurface::AcquireFrame(Size physical_size) {
  @autoreleasepool {
    // 经主线程呈现: 拖拽缩放期间(与窗口尺寸同一事务), 或刚退出平台视图合成、主线程还有待呈现的合成帧时
    // (保持帧顺序);其余时候切回命令缓冲 presentDrawable。
    const bool via_main = live_resizing_.load(std::memory_order_acquire) ||
                          (presents_with_transaction_ && presents_->pending() > 0);
    SetPresentsWithTransaction(via_main);
    MOMO_ASSIGN_OR_RETURN(id<CAMetalDrawable> drawable, NextDrawable(layer_, physical_size));
    platform::SurfaceFrame frame;
    frame.kind = platform::SurfaceKind::kMetal;
    frame.gpu_target = (__bridge void*)drawable;  // 帧内有效: present 闭包持有强引用
    frame.size = {drawable.texture.width * 1.0, drawable.texture.height * 1.0};
    id<MTLCommandQueue> queue = device_->queue;
    std::shared_ptr<PresentQueue> presents = presents_;
    if (via_main) {
      frame.present = [queue, drawable, presents, physical_size]() -> bool {
        @autoreleasepool {
          id<MTLCommandBuffer> marker = [queue commandBuffer];
          if (marker == nil) {
            return false;
          }
          [marker commit];
          [marker waitUntilScheduled];  // 之前提交的绘制命令都已调度
        }
        presents->Post(physical_size, [drawable] {
          [drawable present];  // presentsWithTransaction: 随主线程当前事务提交
        });
        return true;
      };
      return frame;
    }
    frame.present = [queue, drawable, presents, physical_size]() -> bool {
      @autoreleasepool {
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        if (buffer == nil) {
          return false;
        }
        [buffer presentDrawable:drawable];
        [buffer commit];
      }
      presents->NotePresented(physical_size);
      return true;
    };
    return frame;
  }
}

bool MacMetalSurface::SupportsPlatformViews() const {
  return platform_views_ != nil;
}

void MacMetalSurface::SetPlatformViews(MomoPlatformViews* views) { platform_views_ = views; }

CAMetalLayer* MacMetalSurface::OverlayLayer(size_t index) {
  while (overlay_layers_.size() <= index) {
    // raster 线程没有 run loop: 在这里创建/配置图层产生的隐式 CATransaction 永远不会提交,
    // 图层状态到不了渲染服务, 之后呈现给它的 drawable 一律不显示。必须用显式事务包住并立即提交。
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    CAMetalLayer* layer = MakeLayer();
    layer.presentsWithTransaction = YES;  // 与平台视图几何同一事务呈现
    layer.contentsGravity = kCAGravityTopLeft;
    layer.needsDisplayOnBoundsChange = NO;
    [CATransaction commit];
    overlay_layers_.push_back(layer);
  }
  return overlay_layers_[index];
}

StatusOr<platform::CompositionFrame> MacMetalSurface::AcquireCompositionFrame(
    Size physical_size, const platform::CompositionLayout& layout) {
  if (!SupportsPlatformViews()) {
    return UnimplementedError("platform views are not available on this surface");
  }
  @autoreleasepool {
    SetPresentsWithTransaction(true);
    MOMO_ASSIGN_OR_RETURN(id<CAMetalDrawable> base, NextDrawable(layer_, physical_size));
    platform::CompositionFrame frame;
    frame.base.kind = platform::SurfaceKind::kMetal;
    frame.base.gpu_target = (__bridge void*)base;
    frame.base.size = {base.texture.width * 1.0, base.texture.height * 1.0};

    // overlay: 每个非空分片一个独立 CAMetalLayer, drawable 尺寸 = 分片范围。
    std::vector<CAMetalLayer*> layers(layout.overlays.size(), nil);
    std::vector<id<CAMetalDrawable>> drawables;
    frame.overlays.resize(layout.overlays.size());
    for (size_t i = 0; i < layout.overlays.size(); ++i) {
      if (!layout.overlays[i]) {
        continue;
      }
      CAMetalLayer* layer = OverlayLayer(i);
      StatusOr<id<CAMetalDrawable>> drawable =
          NextDrawable(layer, layout.overlays[i]->size());
      if (!drawable.ok()) {
        MOMO_LOG(WARNING) << "Overlay " << i << " dropped: " << drawable.status();
        continue;
      }
      layers[i] = layer;
      drawables.push_back(*drawable);
      platform::SurfaceFrame overlay;
      overlay.kind = platform::SurfaceKind::kMetal;
      overlay.gpu_target = (__bridge void*)*drawable;
      overlay.size = layout.overlays[i]->size();
      frame.overlays[i] = overlay;
    }

    MomoPlatformViews* __weak weak_views = platform_views_;
    std::shared_ptr<PresentQueue> presents = presents_;
    // present(raster 线程): 后端已等到命令被调度;几何与全部 drawable 在主线程同一事务内生效
    // (拖拽缩放时这个事务嵌套在 AppKit 提交新窗口尺寸的事务里)。
    frame.present = [weak_views, presents, physical_size, layout, layers, base, drawables]() -> bool {
      presents->Post(physical_size, [weak_views, layout, layers, base, drawables] {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        MomoPlatformViews* views = weak_views;
        if (views != nil) {
          [views applyLayout:layout overlayLayers:layers];
        }
        [base present];
        for (id<CAMetalDrawable> d : drawables) {
          [d present];
        }
        [CATransaction commit];
      });
      return true;
    };
    return frame;
  }
}

void MacMetalSurface::SetLiveResizing(bool resizing) {
  live_resizing_.store(resizing, std::memory_order_release);
}

void MacMetalSurface::UpdateGeometry() {
  NSView* view = view_;
  if (view == nil || layer_ == nil) {
    return;
  }
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  base_view_.frame = view.bounds;
  layer_.frame = base_view_.layer.bounds;
  const CGFloat scale = view.window ? view.window.backingScaleFactor : view.layer.contentsScale;
  if (scale > 0) {
    layer_.contentsScale = scale;
  }
  [CATransaction commit];
}

}  // namespace momo::embedder::macos
