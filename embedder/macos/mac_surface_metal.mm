#include "embedder/macos/mac_surface_metal.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <optional>

#include "base/blocking_queue.h"
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

// 经主线程呈现的帧: raster 线程入队(帧尺寸 + 呈现动作), 主线程按入队顺序执行。平常由 GCD 主队列触发执行;
// 拖拽缩放时主线程在 WaitForFrame 里直接执行(此时主线程阻塞在 AppKit 的缩放回调中, GCD 任务要等它返回)。
// present 闭包与主线程块共享本对象, surface 先析构也安全。
struct MacMetalSurface::MainThreadPresents {
  struct Entry {
    Size size;
    std::function<void()> present;
  };
  BlockingQueue<Entry> queue;
  std::atomic<int> pending{0};         // 已入队、尚未执行完的帧数(raster 据此保持帧顺序)
  std::atomic<bool> presented{false};  // 是否已呈现过帧(任一路径)

  // raster 线程。
  static void Post(const std::shared_ptr<MainThreadPresents>& self, Size size,
                   std::function<void()> present) {
    self->pending.fetch_add(1);
    self->queue.Push({size, std::move(present)});
    dispatch_async(dispatch_get_main_queue(), ^{
      self->Drain(nullptr);
    });
  }

  // 主线程: 执行一帧。返回它是否是尺寸为 *target 的帧。
  bool Run(Entry& entry, const Size* target) {
    entry.present();
    pending.fetch_sub(1);
    presented.store(true, std::memory_order_release);
    return target != nullptr && static_cast<int>(entry.size.width) == static_cast<int>(target->width) &&
           static_cast<int>(entry.size.height) == static_cast<int>(target->height);
  }

  // 主线程: 依次执行已入队的帧。返回是否执行了尺寸为 *target 的帧。
  bool Drain(const Size* target) {
    bool matched = false;
    while (std::optional<Entry> entry = queue.TryPop()) {
      matched = Run(*entry, target) || matched;
    }
    return matched;
  }
};

StatusOr<std::unique_ptr<MacMetalSurface>> MacMetalSurface::Create(NSView* view) {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    return UnavailableError("No Metal device available");
  }
  id<MTLCommandQueue> queue = [device newCommandQueue];
  if (queue == nil) {
    return UnavailableError("Cannot create MTLCommandQueue");
  }
  return std::unique_ptr<MacMetalSurface>(new MacMetalSurface(view, device, queue));
}

MacMetalSurface::MacMetalSurface(NSView* view, id<MTLDevice> device, id<MTLCommandQueue> queue)
    : view_(view),
      device_(device),
      queue_(queue),
      software_surface_(view),
      presents_(std::make_shared<MainThreadPresents>()) {
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
  layer.device = device_;
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

platform::SurfaceKind MacMetalSurface::kind() const {
  return software_.load(std::memory_order_acquire) ? platform::SurfaceKind::kSoftware : platform::SurfaceKind::kMetal;
}

platform::GpuContextInfo MacMetalSurface::gpu_context() const {
  if (software_.load(std::memory_order_acquire)) {
    return {};
  }
  return {(__bridge void*)device_, (__bridge void*)queue_};
}

bool MacMetalSurface::FallBackToSoftware() {
  if (software_.exchange(true, std::memory_order_acq_rel)) {
    return true;
  }
  MOMO_LOG(WARNING) << "MacMetalSurface: falling back to software presentation.";
  CAMetalLayer* __weak weak_layer = layer_;
  MomoPlatformViews* __weak weak_views = platform_views_;
  dispatch_async(dispatch_get_main_queue(), ^{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [weak_layer removeFromSuperlayer];
    [weak_views hideAll];  // 软件路径不支持平台视图
    [CATransaction commit];
  });
  return true;
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
  if (software_.load(std::memory_order_acquire)) {
    return software_surface_.AcquireFrame(physical_size);
  }
  @autoreleasepool {
    // 经主线程呈现: 拖拽缩放期间(与窗口尺寸同一事务), 或刚退出平台视图合成、主线程还有待呈现的合成帧时
    // (保持帧顺序);其余时候切回命令缓冲 presentDrawable。
    const bool via_main = live_resizing_.load(std::memory_order_acquire) ||
                          (presents_with_transaction_ && presents_->pending.load() > 0);
    SetPresentsWithTransaction(via_main);
    MOMO_ASSIGN_OR_RETURN(id<CAMetalDrawable> drawable, NextDrawable(layer_, physical_size));
    platform::SurfaceFrame frame;
    frame.kind = platform::SurfaceKind::kMetal;
    frame.gpu_target = (__bridge void*)drawable;  // 帧内有效: present 闭包持有强引用
    frame.size = {drawable.texture.width * 1.0, drawable.texture.height * 1.0};
    id<MTLCommandQueue> queue = queue_;
    std::shared_ptr<MainThreadPresents> presents = presents_;
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
        MainThreadPresents::Post(presents, physical_size, [drawable] {
          [drawable present];  // presentsWithTransaction: 随主线程当前事务提交
        });
        return true;
      };
      return frame;
    }
    frame.present = [queue, drawable, presents]() -> bool {
      @autoreleasepool {
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        if (buffer == nil) {
          return false;
        }
        [buffer presentDrawable:drawable];
        [buffer commit];
      }
      presents->presented.store(true, std::memory_order_release);
      return true;
    };
    return frame;
  }
}

bool MacMetalSurface::SupportsPlatformViews() const {
  return !software_.load(std::memory_order_acquire) && platform_views_ != nil;
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
    std::shared_ptr<MainThreadPresents> presents = presents_;
    // present(raster 线程): 后端已等到命令被调度;几何与全部 drawable 在主线程同一事务内生效
    // (拖拽缩放时这个事务嵌套在 AppKit 提交新窗口尺寸的事务里)。
    frame.present = [weak_views, presents, physical_size, layout, layers, base, drawables]() -> bool {
      MainThreadPresents::Post(presents, physical_size, [weak_views, layout, layers, base, drawables] {
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

bool MacMetalSurface::has_presented() const {
  return presents_->presented.load(std::memory_order_acquire);
}

bool MacMetalSurface::WaitForFrame(Size physical_size, TimeDelta timeout,
                                   const std::function<void()>& pump) {
  if (software_.load(std::memory_order_acquire)) {
    return false;
  }
  const TimePoint deadline = Clock::now() + timeout;
  while (true) {
    if (presents_->Drain(&physical_size)) {
      return true;
    }
    const TimePoint now = Clock::now();
    if (now >= deadline) {
      return false;
    }
    // vsync 请求、vsync 回调等平台线程任务: 引擎出下一帧要经过它们。
    pump();
    if (presents_->Drain(&physical_size)) {
      return true;
    }
    const TimeDelta slice = std::min<TimeDelta>(std::chrono::milliseconds(2), deadline - now);
    if (std::optional<MainThreadPresents::Entry> entry = presents_->queue.PopFor(slice)) {
      if (presents_->Run(*entry, &physical_size)) {
        presents_->Drain(nullptr);  // 其后已入队的帧按序一并呈现
        return true;
      }
    }
  }
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
