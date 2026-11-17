#pragma once

// WinD3DSurface: DXGI 交换链 surface(kind kDirect3D;ARCHITECTURE.md §3.5, D3DRasterizer 的平台端, 对应 macOS 的
// MacMetalSurface)。仅被 embedder/windows 的 .cc 包含。
//
// 结构: 交换链不直接绑定 HWND, 而是组合交换链(CreateSwapChainForComposition: flip-model, 2 个 BGRA8 后台缓冲),
// 作为窗口 DirectComposition 树(WinCompositor)上的 visual 内容。主帧一条(不透明);平台视图合成时每个 overlay
// 一条(预乘 alpha, 尺寸 = overlay 的内容范围), 与平台视图的容器 visual 交替叠放(WinPlatformViews::Compose)。
// 设备: ID3D12Device 与 DIRECT 命令队列是应用级的(WinD3DDevice, 平台集成创建一次), 所有窗口的交换链建在同一
//   队列上;引擎据此让全部窗口共用一个 GPU 后端(一份 GrDirectContext, rasterizer.h)。
//
// 线程:
//   - platform 线程: Create / SetPlatformViews / 析构。
//   - raster 线程: 其余全部。后台缓冲的尺寸在取帧时按请求的尺寸调整(ResizeBuffers, raster 线程), resize 不需要
//     跨线程同步;后端每帧画完都已等待 GPU 完成并释放对后台缓冲的引用(D3DRasterizer), 满足 ResizeBuffers 的前提。
// 呈现(都在 raster 线程, 之后报告给窗口的 PresentQueue):
//   - 普通帧: 主交换链 Present(1, 0)。
//   - 平台视图合成帧: 全部交换链 Present, 再在一次 DirectComposition 提交里排好 z 序与视图容器的几何
//     (WinPlatformViews::Compose), 原生视图与 momo 画面在同一个 DWM 帧生效;随后经呈现队列在 platform 线程
//     执行 WinPlatformViews::OnPresented(视图挂入、尺寸、焦点、命中几何)。
//   - 呈现队列里还有待执行的合成帧时, 普通帧也经队列报告, 保持顺序。
// 回退软件: 本类不管——由 FallbackSurface 组合软件 surface。它在 GPU 不可用时调用 Deactivate(): 交换链释放,
//   DirectComposition 树摘下, 平台视图停用, 窗口内容回到 GDI。设备丢失(驱动更新、TDR、显卡切换:
//   DXGI_ERROR_DEVICE_REMOVED/RESET)经 device_lost() 报告, FallbackSurface 据此回退。重建 GPU 设备未实现。
// 拖拽缩放: 组合交换链不随 visual 拉伸——新尺寸的帧到达前, 窗口新露出的部分是闪屏底色(GDI);窗口在 WM_SIZE 里
//   等这个尺寸的帧(PresentQueue::SynchronizeResize), 通常看不到。
// 生命周期: 由 WinWindow 拥有(经 FallbackSurface);Shell 解绑(raster 停用)之后才析构(render_surface.h)。

#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/status.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/fallback_surface.h"
#include "embedder/common/present_queue.h"
#include "embedder/windows/win_util.h"

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Fence;
struct IDXGIFactory4;

namespace momo::embedder::windows {

class WinCompositor;
class WinPlatformViews;

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
  // 交换链创建失败时返回 Unavailable(调用方改用软件 surface)。platform 线程调用。
  // compositor: 窗口的 DirectComposition 树;presents: 窗口的呈现队列。
  static StatusOr<std::unique_ptr<WinD3DSurface>> Create(HWND hwnd, std::shared_ptr<const WinD3DDevice> device,
                                                         std::shared_ptr<WinCompositor> compositor,
                                                         std::shared_ptr<PresentQueue> presents);
  ~WinD3DSurface() override;

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kDirect3D; }
  platform::GpuContextInfo gpu_context() const override;
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;
  bool SupportsPlatformViews() const override;
  StatusOr<platform::CompositionFrame> AcquireCompositionFrame(Size physical_size,
                                                               const platform::CompositionLayout& layout) override;
  void Deactivate() override;
  bool device_lost() const override;

  // platform 线程, 绑定 raster 之前: 平台视图宿主(为空则不支持平台视图)。归窗口所有, 比本对象活得久。
  void SetPlatformViews(WinPlatformViews* views) { platform_views_ = views; }

 private:
  class SwapChain;

  WinD3DSurface(HWND hwnd, std::shared_ptr<const WinD3DDevice> device, std::shared_ptr<WinCompositor> compositor,
                std::shared_ptr<PresentQueue> presents);
  Status Initialize();

  // raster 线程。
  // chain 的后台缓冲调整为 size(需要时先等 GPU 空闲), 返回当前后台缓冲的帧(present 为空)。
  StatusOr<platform::SurfaceFrame> FrameOf(SwapChain& chain, Size size);
  bool Present(SwapChain& chain);
  bool PresentComposition(Size size, const platform::CompositionLayout& layout);
  // 一帧已在本线程呈现: 报告给呈现队列(队列里还有待执行的帧时排在它们之后)。
  void ReportPresented(Size size);
  // 等待队列上已提交的全部工作完成(ResizeBuffers、释放交换链之前)。队列是共用的: 等的是全部窗口的工作,
  // 它们都由 raster 线程提交, 此刻不会有新的。
  void WaitForQueueIdle();
  // 记下设备丢失(之后 device_lost() 为 true)。返回 hr 是否表示设备丢失。
  bool NoteIfDeviceLost(HRESULT hr);

  HWND hwnd_;
  const std::shared_ptr<const WinD3DDevice> device_;
  const std::shared_ptr<WinCompositor> compositor_;
  const std::shared_ptr<PresentQueue> presents_;
  WinPlatformViews* platform_views_ = nullptr;

  Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  uint64_t fence_value_ = 0;                          // raster 线程(析构时 platform 线程, raster 已解绑)
  std::unique_ptr<SwapChain> base_;                   // Deactivate 后为空(raster 线程)
  std::vector<std::unique_ptr<SwapChain>> overlays_;  // 下标与 layout.overlays 对应, 复用(raster 线程)
  bool lost_ = false;                                 // raster 线程

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinD3DSurface);
};

}  // namespace momo::embedder::windows
