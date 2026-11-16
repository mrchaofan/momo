#pragma once

// FallbackSurface: GPU surface 与软件 surface 的组合——"GPU 不可用时回退软件呈现"在这里只实现一次, 两个平台的
// GPU surface(MacMetalSurface / WinD3DSurface)不认识软件 surface, 软件 surface 也不知道自己是不是替补。
//
// GPU 可用时一切转发给 GPU surface。出现下面任一情况后改由软件 surface 提供帧, kind() 随之变为 kSoftware
// (Rasterizer 据此换用软件后端), 不可逆:
//   - Rasterizer 建不出 GPU 后端, 调用 FallBackToSoftware();
//   - GPU surface 报告设备丢失(GpuSurface::device_lost(), 取帧前检查)。
// 回退时 GPU surface 被停用(GpuSurface::Deactivate: 释放交换链 / 移除 CAMetalLayer), 之后不再被调用。
//
// 线程约定: 同 RenderSurface(raster 线程使用);software_active() 任意线程。两个 surface 归本对象所有,
// 生命周期与窗口相同。

#include <atomic>
#include <memory>

#include "base/macros.h"
#include "engine/platform/render_surface.h"

namespace momo::embedder {

// 环境变量 MOMO_SOFTWARE_RENDERING(非空且不为 "0"): 不使用 GPU surface, 窗口直接用软件 surface。
bool SoftwareRenderingForced();

// GPU surface 在回退上需要配合的两点。
class GpuSurface : public engine::platform::RenderSurface {
 public:
  // raster 线程: 停用。释放或隐藏 GPU 呈现资源, 让窗口内容回到软件 surface;之后不再被取帧。
  virtual void Deactivate() = 0;
  // raster 线程: GPU 设备是否已丢失(驱动更新、显卡切换等), 丢失后无法再呈现。
  virtual bool device_lost() const { return false; }
};

class FallbackSurface final : public engine::platform::RenderSurface {
 public:
  FallbackSurface(std::unique_ptr<GpuSurface> gpu,
                  std::unique_ptr<engine::platform::RenderSurface> software);
  ~FallbackSurface() override;

  engine::platform::SurfaceKind kind() const override;
  engine::platform::GpuContextInfo gpu_context() const override;
  bool FallBackToSoftware() override;
  StatusOr<engine::platform::SurfaceFrame> AcquireFrame(Size physical_size) override;
  bool SupportsPlatformViews() const override;
  StatusOr<engine::platform::CompositionFrame> AcquireCompositionFrame(
      Size physical_size, const engine::platform::CompositionLayout& layout) override;

  // 是否已回退到软件呈现。
  bool software_active() const { return software_active_.load(std::memory_order_acquire); }

 private:
  // raster 线程: 设备丢失则回退。返回当前是否由软件 surface 提供帧。
  bool UseSoftware();

  const std::unique_ptr<GpuSurface> gpu_;
  const std::unique_ptr<engine::platform::RenderSurface> software_;
  std::atomic<bool> software_active_{false};

  MOMO_DISALLOW_COPY_AND_ASSIGN(FallbackSurface);
};

}  // namespace momo::embedder
