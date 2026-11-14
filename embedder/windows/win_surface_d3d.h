#pragma once

// WinD3DSurface: DXGI 交换链 surface(kind kDirect3D;docs/05 §4 Direct3DBackend 的平台端, 对应 macOS 的
// MacMetalSurface)。仅被 embedder/windows 的 .cc 包含。
//
// 结构: 窗口(HWND)上的 flip-model 交换链(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2 个 BGRA8 后台缓冲,
// DXGI_SCALING_STRETCH, 不透明)。设备取第一个支持 D3D12 的硬件适配器, DIRECT 命令队列供 Skia 使用。
//
// 线程:
//   - platform 线程: Create / 析构 / Paint / WaitForFrame / has_presented。
//   - raster 线程: kind / gpu_context / AcquireFrame / present / FallBackToSoftware。
//     后台缓冲的尺寸在 AcquireFrame 里按请求的 physical_size 调整(ResizeBuffers, raster 线程), resize 不需要跨线程
//     同步;后端每帧画完都已等待 GPU 完成并释放对后台缓冲的引用(D3DRasterizer), 满足 ResizeBuffers 的前提。
//   - present: IDXGISwapChain::Present(1, 0)(随 vblank)。
// 回退: FallBackToSoftware() 之后 kind() 变为 kSoftware, 帧改由内部的 WinSoftwareSurface 提供;交换链在
//   raster 线程释放(之后窗口内容回到 GDI 绘制)。创建失败(无 D3D12 设备、远程桌面等)时窗口直接用软件 surface。
//   设备丢失(驱动更新、TDR、显卡切换: DXGI_ERROR_DEVICE_REMOVED/RESET)时本对象自行回退软件, Rasterizer 发现
//   kind() 变化后重建后端(rasterizer.cc)。重建 GPU 设备未实现。
// 未在 Windows 上验证的前提: 后端每帧 submit(GrSyncCpu::kYes) 后 Skia 不再引用后台缓冲(ResizeBuffers 的前提);
//   若不成立, ResizeBuffers 失败, 日志会有 "ResizeBuffers failed"。
// 窗口拖拽缩放(同 macOS 的 live resize 同步): WaitForFrame 让 platform 线程在 WM_SIZE 里等一帧新尺寸的帧呈现,
//   新尺寸的窗口与新尺寸的内容同时出现;超时则暂时拉伸旧内容(DXGI_SCALING_STRETCH)。
// 平台视图: 不支持(SupportsPlatformViews() 为 false;Windows 宿主不声明 kCapabilityPlatformViews)。
// 生命周期: 由 WinWindow 拥有;Shell 解绑(raster 停用)之后才析构(render_surface.h)。

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "base/status.h"
#include "base/time.h"
#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_surface_software.h"
#include "embedder/windows/win_util.h"
#include "engine/platform/render_surface.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct ID3D12Fence;
struct IDXGISwapChain3;

namespace momo::embedder::windows {

class WinD3DSurface final : public platform::RenderSurface {
 public:
  // 无可用 D3D12 设备或交换链创建失败时返回 Unavailable(调用方改用 WinSoftwareSurface)。platform 线程调用。
  static StatusOr<std::unique_ptr<WinD3DSurface>> Create(HWND hwnd, uint32_t background_argb);
  ~WinD3DSurface() override;

  platform::SurfaceKind kind() const override;
  platform::GpuContextInfo gpu_context() const override;
  bool FallBackToSoftware() override;
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;

  // platform 线程。
  // 回退后的软件 surface(WinWindow 把 kPresentMessage / WM_PAINT 交给它)。
  WinSoftwareSurface& software() { return software_surface_; }
  bool software_active() const { return software_.load(std::memory_order_acquire); }
  // WM_PAINT: 交换链尚未呈现过帧时填闪屏底色(之后画面由 DWM 合成交换链, 不需要 GDI)。
  void Paint(HDC dc);
  // 等待一帧 physical_size 尺寸的帧呈现(见头部"窗口拖拽缩放")。等待期间以 pump 执行平台线程任务;
  // pump 返回 false 表示本对象已在其中随窗口销毁(立即返回, 不再访问本对象)。
  bool WaitForFrame(Size physical_size, TimeDelta timeout, const std::function<bool()>& pump);
  bool has_presented() const;

 private:
  WinD3DSurface(HWND hwnd, uint32_t background_argb);
  Status Initialize();

  // raster 线程。
  Status ResizeIfNeeded(int width, int height);
  void ReleaseBuffers();
  // 等待队列上已提交的全部工作完成(ResizeBuffers、释放交换链之前)。
  void WaitForQueueIdle();
  bool Present(int width, int height);
  // 设备已丢失时切到软件呈现(raster 线程)。返回是否已丢失。
  bool CheckDeviceLost(HRESULT hr);

  HWND hwnd_;
  WinSoftwareSurface software_surface_;
  std::atomic<bool> software_{false};

  ID3D12Device* device_ = nullptr;           // 拥有(Release)
  ID3D12CommandQueue* queue_ = nullptr;      // 拥有
  IDXGISwapChain3* swap_chain_ = nullptr;    // 拥有;回退后为空(raster 线程)
  ID3D12Fence* fence_ = nullptr;             // 拥有
  HANDLE fence_event_ = nullptr;
  uint64_t fence_value_ = 0;                 // raster 线程(析构时 platform 线程, raster 已解绑)
  ID3D12Resource* buffers_[2] = {nullptr, nullptr};  // 拥有;raster 线程
  int buffer_width_ = 0;                     // raster 线程
  int buffer_height_ = 0;
  // 最近一次成功呈现的帧尺寸(width << 32 | height;0 = 尚未呈现)。raster 写, platform 读。
  std::atomic<uint64_t> presented_size_{0};
};

}  // namespace momo::embedder::windows
