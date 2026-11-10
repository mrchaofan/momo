#pragma once

// WinSoftwareSurface: 软件 surface(kind kSoftware;docs/05 §4 SoftwareBackend 的 Windows 呈现端)。
// 仅被 embedder/windows 的 .cc 包含。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
// raster 线程直接画进本对象持有的内存缓冲(32bpp、预乘 alpha、B,G,R,A 字节序 = platform::SoftwarePixels;
// 窗口不透明, GDI 以 BI_RGB 解释时忽略 alpha 字节)。present 把该缓冲标记为"待显示"并向窗口
// PostMessage(kPresentMessage);platform 线程在 WndProc 中用 StretchDIBits 画到窗口(尺寸一致时为 1:1,
// 拖拽缩放期间先拉伸, 下一帧即修正), WM_PAINT 时重画当前帧(无帧时填闪屏底色)。
//
// 背压(与 MacSoftwareSurface 相同): 主线程最多只挂起一帧——raster 每次 present 只覆盖"待显示"槽,
// 只有槽从空变满时才 PostMessage 一次。raster 比主线程快时丢弃中间帧, 消息队列与内存都有上界。
// 缓冲: 固定 3 个(显示中 / 待显示 / 绘制中), 因此 AcquireFrame 总能拿到空闲缓冲。
//
// 线程: AcquireFrame/present 在 raster 线程;构造/析构、DisplayPending/Paint 在 platform 线程。
// 生命周期: 由 WinWindow 拥有;raster 解绑(Shell::DetachView)后窗口才销毁本对象。投递的消息只携带
// "有新帧"这一信号, 由窗口的 WndProc 转给本对象;窗口销毁时队列中的消息随 HWND 一起被丢弃。
//
// 未实现: docs/05 §5 的"活动 resize 期间 platform 线程阻塞等待该帧 present"(消除拉伸);
//         GPU(ANGLE, kAngleGl)surface —— 无 ANGLE 预编译包, FallBackToSoftware 不需要。

#include <cstdint>
#include <vector>

#include "base/lock.h"
#include "base/macros.h"
#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_util.h"
#include "engine/platform/render_surface.h"

namespace momo::embedder::windows {

class WinSoftwareSurface final : public platform::RenderSurface {
 public:
  // 发往窗口的"有待显示帧"消息(WinWindow 的 WndProc 收到后调用 DisplayPending)。
  static constexpr UINT kPresentMessage = WM_APP + 0x40;

  WinSoftwareSurface(HWND hwnd, uint32_t background_argb);
  ~WinSoftwareSurface() override;

  platform::SurfaceKind kind() const override { return platform::SurfaceKind::kSoftware; }
  StatusOr<platform::SurfaceFrame> AcquireFrame(Size physical_size) override;

  // platform 线程: 取出最新的待显示帧并立即画到窗口。
  void DisplayPending();
  // platform 线程: WM_PAINT(BeginPaint 得到的 dc)。
  void Paint(HDC dc);

 private:
  static constexpr int kBufferCount = 3;

  struct Buffer {
    std::vector<uint32_t> pixels;
    int width = 0;
    int height = 0;
  };

  int PickBuffer() const;  // raster 线程
  bool Present(int index);  // raster 线程
  void Blit(HDC dc, const Buffer& buffer) const;  // platform 线程
  void FillBackground(HDC dc) const;

  HWND hwnd_;
  uint32_t background_argb_;
  // buffers_[i] 的像素: raster 只写"既非待显示也非显示中"的那个;platform 只读 displayed_ 指向的那个。
  Buffer buffers_[kBufferCount];

  mutable Mutex mutex_;
  int pending_ = -1;            // mutex_: 待显示(raster 写, platform 取走)
  int displayed_ = -1;          // mutex_: 显示中(仅 platform 写)
  bool message_posted_ = false;  // mutex_: kPresentMessage 在途

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinSoftwareSurface);
};

}  // namespace momo::embedder::windows
