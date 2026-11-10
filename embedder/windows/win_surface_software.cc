// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。

#include "embedder/windows/win_surface_software.h"

#include <utility>

#include "base/logging.h"

namespace momo::embedder::windows {

namespace {

BITMAPINFO MakeBitmapInfo(int width, int height) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // 负值 = 自上而下(首行在内存开头, 与 platform::SoftwarePixels 一致)
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;  // 32bpp BI_RGB: 内存字节序 B,G,R,X(忽略 alpha)
  return info;
}

}  // namespace

WinSoftwareSurface::WinSoftwareSurface(HWND hwnd, uint32_t background_argb)
    : hwnd_(hwnd), background_argb_(background_argb) {}

WinSoftwareSurface::~WinSoftwareSurface() = default;

StatusOr<platform::SurfaceFrame> WinSoftwareSurface::AcquireFrame(Size physical_size) {
  const int width = static_cast<int>(physical_size.width);
  const int height = static_cast<int>(physical_size.height);
  if (width <= 0 || height <= 0) {
    return FailedPreconditionError("empty surface size");
  }
  const int index = PickBuffer();
  if (index < 0) {
    return UnavailableError("no free software buffer; frame dropped");  // 3 缓冲下不应发生
  }
  Buffer& buffer = buffers_[index];
  if (buffer.width != width || buffer.height != height) {
    // 尺寸变化(resize/换屏): 该缓冲不被 platform 线程引用, 可直接重分配。
    buffer.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
    buffer.width = width;
    buffer.height = height;
  }
  platform::SurfaceFrame frame;
  frame.kind = platform::SurfaceKind::kSoftware;
  frame.software = {buffer.pixels.data(), static_cast<size_t>(width) * 4, width, height};
  frame.size = physical_size;
  frame.present = [this, index] { return Present(index); };
  return frame;
}

int WinSoftwareSurface::PickBuffer() const {
  int pending = -1;
  int displayed = -1;
  {
    MutexLock lock(mutex_);
    pending = pending_;
    displayed = displayed_;
  }
  for (int i = 0; i < kBufferCount; ++i) {
    if (i != pending && i != displayed) {
      return i;
    }
  }
  return -1;
}

bool WinSoftwareSurface::Present(int index) {
  bool post = false;
  {
    MutexLock lock(mutex_);
    pending_ = index;  // 主线程还没来得及显示的旧帧: 直接被覆盖(丢弃)
    if (!message_posted_) {
      message_posted_ = true;
      post = true;
    }
  }
  if (post && !::PostMessageW(hwnd_, kPresentMessage, 0, 0)) {
    MOMO_LOG(ERROR) << "PostMessageW(present) failed: " << ::GetLastError();
    MutexLock lock(mutex_);
    message_posted_ = false;  // 下一帧再试
  }
  return true;
}

void WinSoftwareSurface::DisplayPending() {
  int displayed = -1;
  {
    MutexLock lock(mutex_);
    message_posted_ = false;
    if (pending_ < 0) {
      return;
    }
    displayed_ = pending_;
    pending_ = -1;
    displayed = displayed_;
  }
  HDC dc = ::GetDC(hwnd_);
  if (dc == nullptr) {
    return;
  }
  Blit(dc, buffers_[displayed]);
  ::ReleaseDC(hwnd_, dc);
  // 整个客户区已是最新帧: 取消尚未处理的 WM_PAINT(避免重复绘制)。
  ::ValidateRect(hwnd_, nullptr);
}

void WinSoftwareSurface::Paint(HDC dc) {
  int displayed = -1;
  {
    MutexLock lock(mutex_);
    displayed = displayed_;
  }
  if (displayed < 0) {
    FillBackground(dc);  // 首帧之前: 闪屏底色
    return;
  }
  Blit(dc, buffers_[displayed]);
}

void WinSoftwareSurface::Blit(HDC dc, const Buffer& buffer) const {
  if (buffer.width <= 0 || buffer.height <= 0) {
    return;
  }
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  const int client_width = client.right - client.left;
  const int client_height = client.bottom - client.top;
  if (client_width <= 0 || client_height <= 0) {
    return;  // 最小化
  }
  const BITMAPINFO info = MakeBitmapInfo(buffer.width, buffer.height);
  if (client_width == buffer.width && client_height == buffer.height) {
    ::SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(buffer.width),
                        static_cast<DWORD>(buffer.height), 0, 0, 0,
                        static_cast<UINT>(buffer.height), buffer.pixels.data(), &info,
                        DIB_RGB_COLORS);
  } else {
    // 尺寸不一致(缩放中): 拉伸铺满, 下一帧即为正确尺寸。
    ::SetStretchBltMode(dc, COLORONCOLOR);
    ::StretchDIBits(dc, 0, 0, client_width, client_height, 0, 0, buffer.width, buffer.height,
                    buffer.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
  }
}

void WinSoftwareSurface::FillBackground(HDC dc) const {
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  const COLORREF color = RGB((background_argb_ >> 16) & 0xFF, (background_argb_ >> 8) & 0xFF,
                             background_argb_ & 0xFF);
  HBRUSH brush = ::CreateSolidBrush(color);
  if (brush != nullptr) {
    ::FillRect(dc, &client, brush);
    ::DeleteObject(brush);
  }
}

}  // namespace momo::embedder::windows
