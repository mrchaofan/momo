#include "embedder/windows/win_surface_d3d.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "base/logging.h"

namespace momo::embedder::windows {

namespace {

constexpr UINT kBufferCount = 2;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

template <typename T>
void SafeRelease(T*& object) {
  if (object != nullptr) {
    object->Release();
    object = nullptr;
  }
}

uint64_t PackSize(int width, int height) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) | static_cast<uint32_t>(height);
}

std::string HrString(HRESULT hr) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

}  // namespace

StatusOr<std::unique_ptr<WinD3DSurface>> WinD3DSurface::Create(HWND hwnd, uint32_t background_argb) {
  std::unique_ptr<WinD3DSurface> surface(new WinD3DSurface(hwnd, background_argb));
  MOMO_RETURN_IF_ERROR(surface->Initialize());
  return StatusOr<std::unique_ptr<WinD3DSurface>>(std::move(surface));
}

WinD3DSurface::WinD3DSurface(HWND hwnd, uint32_t background_argb)
    : hwnd_(hwnd), software_surface_(hwnd, background_argb) {}

Status WinD3DSurface::Initialize() {
  IDXGIFactory4* factory = nullptr;
  if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return UnavailableError("CreateDXGIFactory2 failed");
  }
  // 第一个支持 D3D12(特性级别 11_0)的硬件适配器。软件适配器(WARP)不如直接用软件 surface。
  for (UINT i = 0; device_ == nullptr; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
      ::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    }
    adapter->Release();
  }
  if (device_ == nullptr) {
    factory->Release();
    return UnavailableError("No hardware adapter supports Direct3D 12");
  }
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  HRESULT hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_));
  if (FAILED(hr)) {
    factory->Release();
    return UnavailableError("CreateCommandQueue failed: " + HrString(hr));
  }
  hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
  fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(hr) || fence_event_ == nullptr) {
    factory->Release();
    return UnavailableError("CreateFence failed: " + HrString(hr));
  }

  RECT client{};
  ::GetClientRect(hwnd_, &client);
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
  desc.Height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
  desc.Format = kFormat;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = kBufferCount;
  desc.Scaling = DXGI_SCALING_STRETCH;  // 缩放中新帧到达前拉伸旧内容(同 macOS)
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  IDXGISwapChain1* swap_chain1 = nullptr;
  hr = factory->CreateSwapChainForHwnd(queue_, hwnd_, &desc, nullptr, nullptr, &swap_chain1);
  if (SUCCEEDED(hr)) {
    // 全屏由应用决定, 不响应 Alt+Enter。
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    hr = swap_chain1->QueryInterface(IID_PPV_ARGS(&swap_chain_));
    swap_chain1->Release();
  }
  factory->Release();
  if (FAILED(hr) || swap_chain_ == nullptr) {
    return UnavailableError("CreateSwapChainForHwnd failed: " + HrString(hr));
  }
  for (UINT i = 0; i < kBufferCount; ++i) {
    if (FAILED(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) {
      return UnavailableError("IDXGISwapChain::GetBuffer failed");
    }
  }
  buffer_width_ = static_cast<int>(desc.Width);
  buffer_height_ = static_cast<int>(desc.Height);
  return Status::Ok();
}

WinD3DSurface::~WinD3DSurface() {
  // raster 已解绑(Shell::DetachView 先于窗口销毁): 本线程独占全部资源。设备丢失后不等待(fence 不再推进)。
  if (queue_ != nullptr && fence_ != nullptr && device_->GetDeviceRemovedReason() == S_OK) {
    WaitForQueueIdle();
  }
  ReleaseBuffers();
  SafeRelease(swap_chain_);
  SafeRelease(fence_);
  SafeRelease(queue_);
  SafeRelease(device_);
  if (fence_event_ != nullptr) {
    ::CloseHandle(fence_event_);
  }
}

platform::SurfaceKind WinD3DSurface::kind() const {
  return software_active() ? platform::SurfaceKind::kSoftware : platform::SurfaceKind::kDirect3D;
}

platform::GpuContextInfo WinD3DSurface::gpu_context() const {
  if (software_active()) {
    return {};
  }
  return {device_, queue_};
}

bool WinD3DSurface::FallBackToSoftware() {
  MOMO_LOG(WARNING) << "Direct3D surface falling back to software rendering";
  software_.store(true, std::memory_order_release);
  WaitForQueueIdle();
  ReleaseBuffers();
  SafeRelease(swap_chain_);  // 之后窗口内容回到 GDI(WinSoftwareSurface)
  ::InvalidateRect(hwnd_, nullptr, FALSE);
  return true;
}

StatusOr<platform::SurfaceFrame> WinD3DSurface::AcquireFrame(Size physical_size) {
  if (software_active()) {
    return software_surface_.AcquireFrame(physical_size);
  }
  const int width = static_cast<int>(physical_size.width);
  const int height = static_cast<int>(physical_size.height);
  if (width <= 0 || height <= 0) {
    return FailedPreconditionError("empty surface size");
  }
  if (swap_chain_ == nullptr) {
    return FailedPreconditionError("swap chain released");
  }
  if (CheckDeviceLost(device_->GetDeviceRemovedReason())) {
    return software_surface_.AcquireFrame(physical_size);
  }
  MOMO_RETURN_IF_ERROR(ResizeIfNeeded(width, height));
  const UINT index = swap_chain_->GetCurrentBackBufferIndex();
  if (index >= kBufferCount || buffers_[index] == nullptr) {
    return InternalError("invalid back buffer index");
  }
  platform::SurfaceFrame frame;
  frame.kind = platform::SurfaceKind::kDirect3D;
  frame.gpu_target = buffers_[index];
  frame.size = {static_cast<double>(width), static_cast<double>(height)};
  frame.present = [this, width, height] { return Present(width, height); };
  return frame;
}

Status WinD3DSurface::ResizeIfNeeded(int width, int height) {
  if (width == buffer_width_ && height == buffer_height_) {
    return Status::Ok();
  }
  // ResizeBuffers 要求没有任何对后台缓冲的引用、GPU 上也没有使用它们的工作(后端每帧已等待完成)。
  WaitForQueueIdle();
  ReleaseBuffers();
  const HRESULT hr = swap_chain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                                DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr)) {
    buffer_width_ = buffer_height_ = 0;
    CheckDeviceLost(hr);
    return InternalError("IDXGISwapChain::ResizeBuffers failed: " + HrString(hr));
  }
  for (UINT i = 0; i < kBufferCount; ++i) {
    if (FAILED(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) {
      ReleaseBuffers();
      buffer_width_ = buffer_height_ = 0;
      return InternalError("IDXGISwapChain::GetBuffer failed after resize");
    }
  }
  buffer_width_ = width;
  buffer_height_ = height;
  return Status::Ok();
}

void WinD3DSurface::ReleaseBuffers() {
  for (ID3D12Resource*& buffer : buffers_) {
    SafeRelease(buffer);
  }
}

void WinD3DSurface::WaitForQueueIdle() {
  if (queue_ == nullptr || fence_ == nullptr || fence_event_ == nullptr) {
    return;
  }
  const uint64_t value = ++fence_value_;
  if (FAILED(queue_->Signal(fence_, value))) {
    return;
  }
  if (fence_->GetCompletedValue() < value && SUCCEEDED(fence_->SetEventOnCompletion(value, fence_event_))) {
    ::WaitForSingleObject(fence_event_, INFINITE);
  }
}

bool WinD3DSurface::Present(int width, int height) {
  if (swap_chain_ == nullptr) {
    return false;
  }
  const HRESULT hr = swap_chain_->Present(1, 0);
  if (FAILED(hr)) {
    MOMO_LOG(ERROR) << "IDXGISwapChain::Present failed: " << HrString(hr);
    CheckDeviceLost(hr);
    return false;
  }
  presented_size_.store(PackSize(width, height), std::memory_order_release);
  return true;
}

bool WinD3DSurface::CheckDeviceLost(HRESULT hr) {
  if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET && hr != DXGI_ERROR_DEVICE_HUNG &&
      hr != DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
    return false;
  }
  MOMO_LOG(ERROR) << "Direct3D device lost (" << HrString(hr) << "); switching to software rendering";
  // 设备已不可用: 不等待队列(丢失的设备上的 fence 不会再推进), 直接释放交换链。
  software_.store(true, std::memory_order_release);
  ReleaseBuffers();
  SafeRelease(swap_chain_);
  ::InvalidateRect(hwnd_, nullptr, FALSE);
  return true;
}

void WinD3DSurface::Paint(HDC dc) {
  if (software_active() || !has_presented()) {
    software_surface_.Paint(dc);  // 软件帧, 或首帧之前的闪屏底色
  }
}

bool WinD3DSurface::has_presented() const {
  return software_active() ? software_surface_.has_presented()
                           : presented_size_.load(std::memory_order_acquire) != 0;
}

bool WinD3DSurface::WaitForFrame(Size physical_size, TimeDelta timeout, const std::function<bool()>& pump) {
  if (software_active()) {
    return software_surface_.WaitForFrame(physical_size, timeout, pump);
  }
  const uint64_t target = PackSize(static_cast<int>(physical_size.width), static_cast<int>(physical_size.height));
  const TimePoint deadline = Clock::now() + timeout;
  while (presented_size_.load(std::memory_order_acquire) != target) {
    if (Clock::now() >= deadline) {
      return false;
    }
    // vsync 回调等平台线程任务: 引擎出下一帧要经过它们。
    if (!pump()) {
      return false;  // 本对象已随窗口销毁
    }
    ::Sleep(1);  // 拖拽缩放期间窗口把系统定时器精度调到 1 ms(timeBeginPeriod)
  }
  return true;
}

}  // namespace momo::embedder::windows
