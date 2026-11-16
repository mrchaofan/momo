#pragma once

// WinSoftwareSurface: 软件 surface(kind kSoftware;ARCHITECTURE.md §3.5: 软件后端的 Windows 呈现端)。
// 仅被 embedder/windows 的 .cc 包含。
// raster 线程直接画进本对象持有的内存缓冲(32bpp、预乘 alpha、B,G,R,A 字节序 = platform::SoftwarePixels;
// 窗口不透明, GDI 以 BI_RGB 解释时忽略 alpha 字节)。present 把该缓冲标记为"待显示"并入队窗口的 PresentQueue;
// platform 线程执行时用 StretchDIBits 画到窗口(尺寸一致时为 1:1, 拖拽缩放期间先拉伸, 下一帧即修正),
// WM_PAINT 时重画当前帧(无帧时填闪屏底色)。拖拽缩放同步由 PresentQueue 提供(窗口在 WM_SIZE 里等待)。
//
// 背压(与 MacSoftwareSurface 相同): 主线程最多只显示最新的一帧——raster 每次 present 只覆盖"待显示"槽,
// 主线程执行入队的呈现时取走槽里的那一帧。raster 比主线程快时丢弃中间帧, 内存有上界。
// 缓冲: 固定 3 个(显示中 / 待显示 / 绘制中), 因此 AcquireFrame 总能拿到空闲缓冲。
//
// 线程: AcquireFrame/present 在 raster 线程;构造/析构、Paint 在 platform 线程。
// 生命周期: 由 WinWindow 拥有(可能经 FallbackSurface, 作为 D3D 的替补);raster 解绑(Shell::DetachView)后窗口
// 才销毁本对象。入队的呈现引用本对象: 它们只由窗口执行(WndProc 的唤醒消息、WM_SIZE 里的等待), 窗口销毁后
// 不再有人执行, 随队列一起丢弃。

#include <cstdint>
#include <memory>
#include <vector>

#include "base/lock.h"
#include "base/macros.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/present_queue.h"
#include "embedder/windows/win_util.h"
#include "engine/platform/render_surface.h"

namespace momo::embedder::windows {

class WinSoftwareSurface final : public platform::RenderSurface {
 public:
  // presents: 窗口的呈现队列。
  WinSoftwareSurface(HWND hwnd, uint32_t background_argb, std::shared_ptr<PresentQueue> presents);
  ~WinSoftwareSurface() override;

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;

  // platform 线程: WM_PAINT(BeginPaint 得到的 dc)。重画当前帧;还没有帧时填闪屏底色。
  void Paint(HDC dc);

 private:
  static constexpr int kBufferCount = 3;

  struct Buffer {
    std::vector<uint32_t> pixels;
    int width = 0;
    int height = 0;
  };

  int PickBuffer() const;  // raster 线程
  bool Present(int index, Size size);  // raster 线程
  void DisplayPending();  // platform 线程: 取出最新的待显示帧并立即画到窗口
  void Blit(HDC dc, const Buffer& buffer) const;  // platform 线程
  void FillBackground(HDC dc) const;

  HWND hwnd_;
  uint32_t background_argb_;
  const std::shared_ptr<PresentQueue> presents_;
  // buffers_[i] 的像素: raster 只写"既非待显示也非显示中"的那个;platform 只读 displayed_ 指向的那个。
  Buffer buffers_[kBufferCount];

  mutable Mutex mutex_;
  int pending_ = -1;            // mutex_: 待显示(raster 写, platform 取走)
  int displayed_ = -1;          // mutex_: 显示中(仅 platform 写)

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinSoftwareSurface);
};

}  // namespace momo::embedder::windows
