#pragma once

// MacMetalSurface: CAMetalLayer surface(docs/03 §1、docs/05 §4 MetalBackend 的平台端)。仅被 .mm 包含。
//
// 结构: 视图保持 layer-backed(普通 CALayer, 软件回退时 contents 放 IOSurface);CAMetalLayer 挂在一个
// 专用的底层子视图(内容视图的第一个子视图)里铺满视图。层级用 AppKit 的子视图顺序表达(平台视图宿主
// 在其后加入, 位于其上), 不依赖 zPosition: AppKit 不按 zPosition 排列视图的 backing layer 与手动子层。pixelFormat = BGRA8Unorm, framebufferOnly = NO(Skia 需要采样/拷贝), colorspace = sRGB。
//
// 线程:
//   - platform 线程: 构造/析构、UpdateGeometry(resize/DPI 变化时同步 frame 与 contentsScale)。
//   - raster 线程: kind/gpu_context/AcquireFrame/present/FallBackToSoftware。
//     drawableSize 在 AcquireFrame 中按请求的 physical_size 设置(raster 线程, 显式 CATransaction),
//     因此 resize 不需要跨线程同步 drawableSize;nextDrawable 可在后台线程调用。
//   - present: 在同一 MTLCommandQueue 上新建命令缓冲 presentDrawable + commit, 排在后端已提交的
//     绘制命令之后。
// 回退: FallBackToSoftware() 之后 kind() 变为 kSoftware, 帧改由内部的 MacSoftwareSurface 提供;
//       CAMetalLayer 在主线程(弱引用)移除。
// 平台视图合成(docs/05 §9): AcquireCompositionFrame 给出主帧 + 各 overlay(独立 CAMetalLayer, raster
//   线程创建、主线程挂进 MomoPlatformViews 的 overlay 视图)的 drawable;present 把"平台视图几何更新 +
//   全部 drawable 呈现"投递到主线程的同一个 CATransaction(layer.presentsWithTransaction = YES), 使原生
//   视图与 momo 画面同帧生效。退出合成后, 主线程仍有待呈现的帧时普通帧也经主线程呈现, 保证顺序;
//   之后切回命令缓冲 presentDrawable。
// 生命周期: 由 MacWindow 拥有;Shell 解绑(raster 停用)之后才析构(render_surface.h)。

#include <atomic>
#include <memory>

#include "embedder/common/engine_platform.h"
#include "embedder/macos/mac_surface_software.h"
#include "engine/platform/render_surface.h"

#include <vector>

#ifdef __OBJC__
@class NSView;
@class CAMetalLayer;
@class MomoPlatformViews;
@protocol CAMetalDrawable;
@protocol MTLDevice;
@protocol MTLCommandQueue;
#endif

namespace momo::embedder::macos {

class MacMetalSurface final : public platform::RenderSurface {
 public:
  // 无可用 Metal 设备时返回 Unavailable(调用方改用 MacSoftwareSurface)。platform 线程调用。
  static StatusOr<std::unique_ptr<MacMetalSurface>> Create(NSView* view);
  ~MacMetalSurface() override;

  platform::SurfaceKind kind() const override;
  platform::GpuContextInfo gpu_context() const override;
  bool FallBackToSoftware() override;
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;
  bool SupportsPlatformViews() const override;
  StatusOr<platform::CompositionFrame> AcquireCompositionFrame(
      Size physical_size, const platform::CompositionLayout& layout) override;

  // platform 线程: 视图尺寸或 backingScaleFactor 变化后调用。
  void UpdateGeometry();
  // platform 线程, 绑定 raster 之前: 平台视图宿主(弱引用;为空则不支持平台视图)。
  void SetPlatformViews(MomoPlatformViews* views);

 private:
  MacMetalSurface(NSView* view, id<MTLDevice> device, id<MTLCommandQueue> queue);

  // raster 线程。
  CAMetalLayer* MakeLayer() const;
  CAMetalLayer* OverlayLayer(size_t index);
  void SetPresentsWithTransaction(bool value);
  StatusOr<id<CAMetalDrawable>> NextDrawable(CAMetalLayer* layer, Size physical_size);

  NSView* __weak view_;
  NSView* base_view_;  // CAMetalLayer 的宿主(内容视图的第一个子视图)
  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  CAMetalLayer* layer_;
  std::atomic<bool> software_{false};
  MacSoftwareSurface software_surface_;
  MomoPlatformViews* __weak platform_views_ = nil;
  // 仅 raster 线程。
  std::vector<CAMetalLayer*> overlay_layers_;
  bool presents_with_transaction_ = false;
  // 已投递到主线程、尚未呈现的帧数(raster 写 +1, 主线程 -1)。
  std::shared_ptr<std::atomic<int>> pending_main_presents_ = std::make_shared<std::atomic<int>>(0);
};

}  // namespace momo::embedder::macos
