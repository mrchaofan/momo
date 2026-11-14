#include "embedder/macos/mac_surface_software.h"

#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include "base/lock.h"

namespace momo::embedder::macos {

namespace {

constexpr size_t kMaxBuffers = 3;

IOSurfaceRef CreateBuffer(int width, int height) {
  NSDictionary* props = @{
    (id)kIOSurfaceWidth : @(width),
    (id)kIOSurfaceHeight : @(height),
    (id)kIOSurfaceBytesPerElement : @4,
    (id)kIOSurfaceBytesPerRow :
        @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, static_cast<size_t>(width) * 4)),
    (id)kIOSurfacePixelFormat : @(static_cast<uint32_t>('BGRA')),
  };
  IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
  if (surface == nullptr) {
    return nullptr;
  }
  // 像素按 sRGB 解释(与 Skia 输出一致), 由 WindowServer 匹配到显示器色域。
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CFPropertyListRef plist = CGColorSpaceCopyPropertyList(srgb);
  if (plist != nullptr) {
    IOSurfaceSetValue(surface, kIOSurfaceColorSpace, plist);
    CFRelease(plist);
  }
  CGColorSpaceRelease(srgb);
  return surface;
}

}  // namespace

// raster 线程与主线程共享的呈现状态。
struct MacSoftwareSurface::PresentState {
  ~PresentState() {
    if (pending != nullptr) CFRelease(pending);
    if (displayed != nullptr) CFRelease(displayed);
  }
  Mutex lock;
  IOSurfaceRef pending = nullptr;    // 待主线程显示(持有引用)
  IOSurfaceRef displayed = nullptr;  // 当前 layer.contents(持有引用)
  bool dispatch_scheduled = false;
  NSView* __weak view = nil;

  // 主线程: 取出最新的待显示帧并设为 layer contents。
  static void DisplayPending(const std::shared_ptr<PresentState>& state) {
    IOSurfaceRef surface = nullptr;
    {
      MutexLock scope(state->lock);
      surface = state->pending;
      state->pending = nullptr;
      state->dispatch_scheduled = false;
    }
    if (surface == nullptr) {
      return;
    }
    NSView* view = state->view;
    if (view != nil) {
      [CATransaction begin];
      [CATransaction setDisableActions:YES];  // 不要隐式淡入动画
      view.layer.contents = (__bridge id)surface;
      [CATransaction commit];
    }
    MutexLock scope(state->lock);
    if (state->displayed != nullptr) {
      CFRelease(state->displayed);
    }
    state->displayed = surface;  // 接管 pending 的引用
  }
};

MacSoftwareSurface::MacSoftwareSurface(NSView* view) : state_(std::make_shared<PresentState>()) {
  state_->view = view;
}

MacSoftwareSurface::~MacSoftwareSurface() {
  for (IOSurfaceRef surface : buffers_) {
    CFRelease(surface);
  }
}

StatusOr<platform::SurfaceFrame> MacSoftwareSurface::AcquireFrame(Size physical_size) {
  const int width = static_cast<int>(physical_size.width);
  const int height = static_cast<int>(physical_size.height);
  if (width <= 0 || height <= 0) {
    return FailedPreconditionError("empty surface size");
  }
  IOSurfaceRef target = PickBuffer(width, height);
  if (target == nullptr) {
    return UnavailableError("all IOSurface buffers are busy; frame dropped");
  }
  if (IOSurfaceLock(target, 0, nullptr) != kIOReturnSuccess) {
    return InternalError("IOSurfaceLock failed");
  }
  platform::SurfaceFrame frame;
  frame.kind = platform::SurfaceKind::kSoftware;
  frame.software = {IOSurfaceGetBaseAddress(target), IOSurfaceGetBytesPerRow(target), width,
                    height};
  frame.size = physical_size;
  frame.present = [this, target] { return Present(target); };
  return frame;
}

// 选一个空闲缓冲: 尺寸匹配、未被 WindowServer 使用、不是待显示/显示中的那个。
IOSurfaceRef MacSoftwareSurface::PickBuffer(int width, int height) {
  // 尺寸变化(resize/换屏)后旧缓冲作废;仍被 CA 引用的会在它释放后自然销毁。
  for (auto it = buffers_.begin(); it != buffers_.end();) {
    if (static_cast<int>(IOSurfaceGetWidth(*it)) != width ||
        static_cast<int>(IOSurfaceGetHeight(*it)) != height) {
      CFRelease(*it);
      it = buffers_.erase(it);
    } else {
      ++it;
    }
  }
  IOSurfaceRef pending = nullptr;
  IOSurfaceRef displayed = nullptr;
  {
    MutexLock scope(state_->lock);
    pending = state_->pending;
    displayed = state_->displayed;
  }
  for (IOSurfaceRef surface : buffers_) {
    if (surface != pending && surface != displayed && !IOSurfaceIsInUse(surface)) {
      return surface;
    }
  }
  if (buffers_.size() < kMaxBuffers) {
    if (IOSurfaceRef surface = CreateBuffer(width, height)) {
      buffers_.push_back(surface);
      return surface;
    }
  }
  return nullptr;
}

bool MacSoftwareSurface::Present(IOSurfaceRef surface) {
  IOSurfaceUnlock(surface, 0, nullptr);
  bool schedule = false;
  {
    MutexLock scope(state_->lock);
    if (state_->pending != nullptr) {
      CFRelease(state_->pending);  // 主线程还没来得及显示的旧帧: 直接丢弃
    }
    CFRetain(surface);
    state_->pending = surface;
    schedule = !state_->dispatch_scheduled;
    state_->dispatch_scheduled = true;
  }
  if (schedule) {
    std::shared_ptr<PresentState> state = state_;
    dispatch_async(dispatch_get_main_queue(), ^{
      PresentState::DisplayPending(state);
    });
  }
  return true;
}

}  // namespace momo::embedder::macos
