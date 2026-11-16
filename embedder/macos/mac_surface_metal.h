#pragma once

// MacMetalSurface: CAMetalLayer surface(ARCHITECTURE.md §3.5;engine 侧是 MetalRasterizer)。仅被 .mm 包含。
//
// 结构: 视图保持 layer-backed(普通 CALayer, 软件回退时 contents 放 IOSurface);CAMetalLayer 挂在一个
// 专用的底层子视图(MomoSurfaceBaseView, 内容视图的第一个子视图)里铺满视图。层级只用 AppKit 的子视图顺序
// 表达(平台视图宿主在其后加入, 位于其上), 不用 zPosition。
// pixelFormat = BGRA8Unorm, framebufferOnly = NO(Skia 需要采样/拷贝), colorspace = sRGB。
//
// 设备: MTLDevice 与 MTLCommandQueue 是应用级的(MacMetalDevice, 平台集成创建一次), 所有窗口的 surface 共用;
//   引擎据此让全部窗口共用一个 GPU 后端(一份 GrDirectContext, rasterizer.h)。
// 线程:
//   - platform 线程: 构造/析构、UpdateGeometry(resize/DPI 变化时同步 frame 与 contentsScale)。
//   - raster 线程: kind/gpu_context/AcquireFrame/present/Deactivate。
//     drawableSize 在 AcquireFrame 中按请求的 physical_size 设置(raster 线程, 显式 CATransaction),
//     因此 resize 不需要跨线程同步 drawableSize;nextDrawable 可在后台线程调用。
// 呈现(两种, 都报告给窗口的 PresentQueue):
//   - 平常: raster 线程在同一 MTLCommandQueue 上新建命令缓冲 presentDrawable + commit, 排在后端已提交的
//     绘制命令之后。
//   - 经主线程(layer.presentsWithTransaction = YES;入队 PresentQueue, 主线程在当前 CATransaction 里
//     [drawable present]): 平台视图合成帧——"平台视图几何更新 + 主帧与各 overlay(独立 CAMetalLayer, raster
//     线程创建、主线程挂进 MomoPlatformViews 的 overlay 视图)呈现"同一事务生效, 原生视图与 momo 画面同帧;
//     拖拽缩放期间的全部帧——与窗口的新尺寸同一事务提交(PresentQueue::WaitForFrame)。
//     退出合成后主线程仍有待呈现的帧时, 普通帧也经主线程呈现以保持顺序, 之后切回命令缓冲。
// 回退软件: 本类不管——由 FallbackSurface 组合软件 surface;它在 GPU 不可用时调用 Deactivate(), CAMetalLayer
//   在主线程(弱引用)移除, 平台视图隐藏。
// 生命周期: 由 MacWindow 拥有(经 FallbackSurface);Shell 解绑(raster 停用)之后才析构(render_surface.h)。

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/fallback_surface.h"
#include "embedder/common/present_queue.h"

#ifdef __OBJC__
@class NSView;
@class CAMetalLayer;
@class MomoPlatformViews;
@protocol CAMetalDrawable;
@protocol MTLDevice;
@protocol MTLCommandQueue;
#endif

namespace momo::embedder::macos {

// 应用级的 Metal 设备与命令队列: 平台集成创建一次, 交给每个窗口的 surface。不可变, 任意线程可用
// (MTLCommandQueue 线程安全)。
struct MacMetalDevice {
  // 无可用 Metal 设备时返回 Unavailable(调用方改用软件 surface)。
  static StatusOr<std::shared_ptr<const MacMetalDevice>> Create();

  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
};

class MacMetalSurface final : public GpuSurface {
 public:
  // platform 线程。presents: 窗口的呈现队列。
  MacMetalSurface(NSView* view, std::shared_ptr<const MacMetalDevice> device,
                  std::shared_ptr<PresentQueue> presents);
  ~MacMetalSurface() override;

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kMetal; }
  platform::GpuContextInfo gpu_context() const override;
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;
  bool SupportsPlatformViews() const override;
  StatusOr<platform::CompositionFrame> AcquireCompositionFrame(
      Size physical_size, const platform::CompositionLayout& layout) override;
  void Deactivate() override;

  // platform 线程: 视图尺寸或 backingScaleFactor 变化后调用。
  void UpdateGeometry();
  // platform 线程, 绑定 raster 之前: 平台视图宿主(弱引用;为空则不支持平台视图)。
  void SetPlatformViews(MomoPlatformViews* views);
  // platform 线程: 窗口开始/结束拖拽缩放。期间全部帧经主线程呈现, 与窗口尺寸同一事务提交。
  void SetLiveResizing(bool resizing);

 private:
  // raster 线程。
  CAMetalLayer* MakeLayer() const;
  CAMetalLayer* OverlayLayer(size_t index);
  void SetPresentsWithTransaction(bool value);
  StatusOr<id<CAMetalDrawable>> NextDrawable(CAMetalLayer* layer, Size physical_size);

  NSView* __weak view_;
  NSView* base_view_;  // CAMetalLayer 的宿主(内容视图的第一个子视图)
  const std::shared_ptr<const MacMetalDevice> device_;
  const std::shared_ptr<PresentQueue> presents_;
  CAMetalLayer* layer_;
  MomoPlatformViews* __weak platform_views_;  // ARC: 初始为 nil
  // 仅 raster 线程。
  std::vector<CAMetalLayer*> overlay_layers_;
  bool presents_with_transaction_ = false;
  std::atomic<bool> live_resizing_{false};
};

}  // namespace momo::embedder::macos
