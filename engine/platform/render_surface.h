#pragma once

// RenderSurface: 渲染表面(raster 线程使用)。由 PlatformWindow 拥有, 生命周期不长于窗口。

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "base/geometry.h"
#include "base/status.h"
#include "engine/platform/platform_view.h"

namespace momo::engine::platform {

enum class SurfaceKind { kMetal, kDirect3D, kSoftware, kNone };

// 软件帧的像素缓冲: 32bpp、预乘 alpha、内存字节序 B,G,R,A
// (= Skia kBGRA_8888_SkColorType / CoreGraphics kCGImageAlphaPremultipliedFirst | ByteOrder32Little)。
struct SoftwarePixels {
  void* pixels = nullptr;
  size_t row_bytes = 0;
  int width = 0;
  int height = 0;
};

// 一帧的绘制目标。present 在 raster 线程调用, 返回 false 表示呈现失败。
//   kSoftware: software 有效。
//   kMetal:    gpu_target = id<CAMetalDrawable>(非拥有, 帧内有效), GPU 后端把它包装为 render target;
//              present 负责 presentDrawable(后端在 present 前已提交命令缓冲)。
//   kDirect3D: gpu_target = ID3D12Resource*(swapchain 的当前后台缓冲, 非拥有, 帧内有效, 状态为
//              D3D12_RESOURCE_STATE_PRESENT);后端画完后把它转回 PRESENT 并等待 GPU 完成, present 负责
//              IDXGISwapChain::Present。
struct SurfaceFrame {
  SurfaceKind kind = SurfaceKind::kNone;
  SoftwarePixels software;      // kind == kSoftware
  void* gpu_target = nullptr;
  Size size;                    // 帧的物理像素尺寸(GPU 帧必填)
  std::function<bool()> present;
};

// GPU surface 的设备信息(raster 线程上取用, 在 surface 存活期间有效)。
//   kMetal:   device = id<MTLDevice>, queue = id<MTLCommandQueue>
//   kDirect3D: device = ID3D12Device*, queue = ID3D12CommandQueue*(DIRECT 队列)
struct GpuContextInfo {
  void* device = nullptr;
  void* queue = nullptr;
};

// 平台视图合成帧(platform_view.h): 一个主帧 + 每个非空 overlay 一帧, 由 present 原子呈现。
//   base / overlays[i] 的 present 均为空(不单独呈现);overlays[i] 的帧尺寸 = layout.overlays[i] 的大小,
//   帧内像素 (0,0) 对应设备坐标 layout.overlays[i]->x/y。
//   present(raster 线程调用)负责: 平台视图几何更新 + 主帧与全部 overlay 呈现, 同一时刻生效
//   (macOS: 主线程同一个 CATransaction)。返回 false 表示呈现失败。
struct CompositionFrame {
  SurfaceFrame base;
  std::vector<std::optional<SurfaceFrame>> overlays;  // 与 layout.overlays 一一对应
  std::function<bool()> present;
};

class RenderSurface {
 public:
  virtual ~RenderSurface() = default;
  virtual SurfaceKind kind() const = 0;
  // GPU surface 返回设备/队列;软件 surface 返回空。
  virtual GpuContextInfo gpu_context() const { return {}; }
  // GPU 初始化失败时由 Rasterizer 调用: 让 surface 改为软件呈现(kind() 随之变为 kSoftware)。
  // 返回 false 表示不支持回退。
  virtual bool FallBackToSoftware() { return false; }
  // physical_size: 物理像素。软件 surface 返回的缓冲在下一次 AcquireFrame 前有效。
  virtual StatusOr<SurfaceFrame> AcquireFrame(Size physical_size) = 0;

  // ---- 平台视图合成(可选能力, docs/05 §9)----
  // 不支持时平台视图不显示(PlatformViewLayer 不绘制任何内容), 其余画面照常。
  // 返回值可随回退变化(如 Metal 回退软件后变为 false), raster 线程每帧查询。
  virtual bool SupportsPlatformViews() const { return false; }
  // 取一帧合成目标(raster 线程)。layout.views 为空表示"本帧没有平台视图"(用于从合成模式退出:
  // 实现方隐藏全部平台视图与 overlay)。
  virtual StatusOr<CompositionFrame> AcquireCompositionFrame(Size physical_size,
                                                             const CompositionLayout& layout) {
    (void)physical_size;
    (void)layout;
    return UnimplementedError("platform views are not supported by this surface");
  }
};

}  // namespace momo::engine::platform
