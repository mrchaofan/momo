#pragma once

// WinD3DSurface: DXGI 交换链 surface(kind kDirect3D;docs/05 §4 Direct3DBackend 的平台端, 对应 macOS 的
// MacMetalSurface)。仅被 embedder/windows 的 .cc 包含。
//
// 结构: 窗口(HWND)上的 flip-model 交换链(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2 个 BGRA8 后台缓冲,
// DXGI_SCALING_STRETCH, 不透明)。
// 设备: ID3D12Device 与 DIRECT 命令队列是应用级的(WinD3DDevice, 平台集成创建一次), 所有窗口的交换链建在同一
//   队列上;引擎据此让全部窗口共用一个 GPU 后端(一份 GrDirectContext, rasterizer.h)。
//
// 线程:
//   - platform 线程: Create / 析构。
//   - raster 线程: kind / gpu_context / AcquireFrame / present / Deactivate / device_lost。
//     后台缓冲的尺寸在 AcquireFrame 里按请求的 physical_size 调整(ResizeBuffers, raster 线程), resize 不需要跨线程
//     同步;后端每帧画完都已等待 GPU 完成并释放对后台缓冲的引用(D3DRasterizer), 满足 ResizeBuffers 的前提。
//   - present: IDXGISwapChain::Present(1, 0)(随 vblank), 在 raster 线程完成, 之后报告给窗口的 PresentQueue
//     (拖拽缩放时窗口在 WM_SIZE 里等这个尺寸的帧, PresentQueue::WaitForFrame)。
// 回退软件: 本类不管——由 FallbackSurface 组合软件 surface。它在 GPU 不可用时调用 Deactivate(): 交换链在
//   raster 线程释放, 窗口内容回到 GDI 绘制。设备丢失(驱动更新、TDR、显卡切换: DXGI_ERROR_DEVICE_REMOVED/RESET)
//   经 device_lost() 报告, FallbackSurface 据此回退。重建 GPU 设备未实现。
// 未在 Windows 上验证的前提: 后端每帧 submit(GrSyncCpu::kYes) 后 Skia 不再引用后台缓冲(ResizeBuffers 的前提);
//   若不成立, ResizeBuffers 失败, 日志会有 "ResizeBuffers failed"。
// 平台视图: 不支持(SupportsPlatformViews() 为 false)。
// 生命周期: 由 WinWindow 拥有(经 FallbackSurface);Shell 解绑(raster 停用)之后才析构(render_surface.h)。

#include <cstdint>
#include <memory>

#include "base/macros.h"
#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/fallback_surface.h"
#include "embedder/common/present_queue.h"
#include "embedder/windows/win_util.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
struct ID3D12Fence;
struct IDXGISwapChain3;

namespace momo::embedder::windows {

// 应用级的 Direct3D 12 设备与 DIRECT 命令队列: 平台集成创建一次, 交给每个窗口的 surface。创建后不可变,
// 任意线程可用(D3D12 设备与命令队列线程安全)。
class WinD3DDevice final {
 public:
  // 取第一个支持 D3D12 的硬件适配器(软件适配器 WARP 不如直接用软件 surface)。没有时返回 Unavailable
  // (远程桌面等;调用方改用软件 surface)。
  static StatusOr<std::shared_ptr<const WinD3DDevice>> Create();
  ~WinD3DDevice();

  ID3D12Device* device() const { return device_; }
  ID3D12CommandQueue* queue() const { return queue_; }

 private:
  WinD3DDevice() = default;

  ID3D12Device* device_ = nullptr;       // 拥有(Release)
  ID3D12CommandQueue* queue_ = nullptr;  // 拥有

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinD3DDevice);
};

class WinD3DSurface final : public GpuSurface {
 public:
  // 交换链创建失败时返回 Unavailable(调用方改用软件 surface)。platform 线程调用。presents: 窗口的呈现队列。
  static StatusOr<std::unique_ptr<WinD3DSurface>> Create(HWND hwnd,
                                                         std::shared_ptr<const WinD3DDevice> device,
                                                         std::shared_ptr<PresentQueue> presents);
  ~WinD3DSurface() override;

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kDirect3D; }
  platform::GpuContextInfo gpu_context() const override;
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;
  void Deactivate() override;
  bool device_lost() const override;

 private:
  WinD3DSurface(HWND hwnd, std::shared_ptr<const WinD3DDevice> device,
                std::shared_ptr<PresentQueue> presents);
  Status Initialize();

  // raster 线程。
  Status ResizeIfNeeded(int width, int height);
  void ReleaseBuffers();
  // 等待队列上已提交的全部工作完成(ResizeBuffers、释放交换链之前)。队列是共用的: 等的是全部窗口的工作,
  // 它们都由 raster 线程提交, 此刻不会有新的。
  void WaitForQueueIdle();
  bool Present(Size size);
  // 记下设备丢失(之后 device_lost() 为 true)。返回 hr 是否表示设备丢失。
  bool NoteIfDeviceLost(HRESULT hr);

  HWND hwnd_;
  const std::shared_ptr<const WinD3DDevice> device_;
  const std::shared_ptr<PresentQueue> presents_;

  IDXGISwapChain3* swap_chain_ = nullptr;    // 拥有;Deactivate 后为空(raster 线程)
  ID3D12Fence* fence_ = nullptr;             // 拥有
  HANDLE fence_event_ = nullptr;
  uint64_t fence_value_ = 0;                 // raster 线程(析构时 platform 线程, raster 已解绑)
  ID3D12Resource* buffers_[2] = {nullptr, nullptr};  // 拥有;raster 线程
  int buffer_width_ = 0;                     // raster 线程
  int buffer_height_ = 0;
  bool lost_ = false;                        // raster 线程
};

}  // namespace momo::embedder::windows
