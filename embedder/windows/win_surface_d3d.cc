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

std::string HrString(HRESULT hr) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

bool IsDeviceLost(HRESULT hr) {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
         hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

}  // namespace

// ---------------------------------------------------------------------------
// WinD3DDevice

StatusOr<std::shared_ptr<const WinD3DDevice>> WinD3DDevice::Create() {
  IDXGIFactory4* factory = nullptr;
  if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return UnavailableError("CreateDXGIFactory2 failed");
  }
  std::shared_ptr<WinD3DDevice> shared(new WinD3DDevice());
  for (UINT i = 0; shared->device_ == nullptr; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
      ::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&shared->device_));
    }
    adapter->Release();
  }
  factory->Release();
  if (shared->device_ == nullptr) {
    return UnavailableError("No hardware adapter supports Direct3D 12");
  }
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  const HRESULT hr = shared->device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&shared->queue_));
  if (FAILED(hr)) {
    return UnavailableError("CreateCommandQueue failed: " + HrString(hr));
  }
  return std::shared_ptr<const WinD3DDevice>(std::move(shared));
}

WinD3DDevice::~WinD3DDevice() {
  SafeRelease(queue_);
  SafeRelease(device_);
}

// ---------------------------------------------------------------------------
// WinD3DSurface

StatusOr<std::unique_ptr<WinD3DSurface>> WinD3DSurface::Create(HWND hwnd,
                                                               std::shared_ptr<const WinD3DDevice> device,
                                                               std::shared_ptr<PresentQueue> presents) {
  std::unique_ptr<WinD3DSurface> surface(new WinD3DSurface(hwnd, std::move(device), std::move(presents)));
  MOMO_RETURN_IF_ERROR(surface->Initialize());
  return StatusOr<std::unique_ptr<WinD3DSurface>>(std::move(surface));
}

WinD3DSurface::WinD3DSurface(HWND hwnd, std::shared_ptr<const WinD3DDevice> device,
                             std::shared_ptr<PresentQueue> presents)
    : hwnd_(hwnd), device_(std::move(device)), presents_(std::move(presents)) {}

Status WinD3DSurface::Initialize() {
  IDXGIFactory4* factory = nullptr;
  if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    return UnavailableError("CreateDXGIFactory2 failed");
  }
  HRESULT hr = device_->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
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
  hr = factory->CreateSwapChainForHwnd(device_->queue(), hwnd_, &desc, nullptr, nullptr, &swap_chain1);
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
  // raster 已解绑(Shell::DetachView 先于窗口销毁): 本线程独占交换链。设备丢失后不等待(fence 不再推进)。
  if (swap_chain_ != nullptr && !device_lost()) {
    WaitForQueueIdle();
  }
  ReleaseBuffers();
  SafeRelease(swap_chain_);
  SafeRelease(fence_);
  if (fence_event_ != nullptr) {
    ::CloseHandle(fence_event_);
  }
}

platform::GpuContextInfo WinD3DSurface::gpu_context() const {
  return {device_->device(), device_->queue()};
}

bool WinD3DSurface::device_lost() const {
  return lost_ || device_->device()->GetDeviceRemovedReason() != S_OK;
}

void WinD3DSurface::Deactivate() {
  if (!device_lost()) {
    WaitForQueueIdle();  // 丢失的设备上 fence 不会再推进: 不等待
  }
  ReleaseBuffers();
  SafeRelease(swap_chain_);  // 之后窗口内容回到 GDI(软件 surface)
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

StatusOr<platform::SurfaceFrame> WinD3DSurface::AcquireFrame(Size physical_size) {
  const int width = static_cast<int>(physical_size.width);
  const int height = static_cast<int>(physical_size.height);
  if (width <= 0 || height <= 0) {
    return FailedPreconditionError("empty surface size");
  }
  if (swap_chain_ == nullptr) {
    return FailedPreconditionError("swap chain released");
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
  frame.present = [this, size = frame.size] { return Present(size); };
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
    NoteIfDeviceLost(hr);
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
  if (fence_ == nullptr || fence_event_ == nullptr) {
    return;
  }
  const uint64_t value = ++fence_value_;
  if (FAILED(device_->queue()->Signal(fence_, value))) {
    return;
  }
  if (fence_->GetCompletedValue() < value && SUCCEEDED(fence_->SetEventOnCompletion(value, fence_event_))) {
    ::WaitForSingleObject(fence_event_, INFINITE);
  }
}

bool WinD3DSurface::Present(Size size) {
  if (swap_chain_ == nullptr) {
    return false;
  }
  const HRESULT hr = swap_chain_->Present(1, 0);
  if (FAILED(hr)) {
    MOMO_LOG(ERROR) << "IDXGISwapChain::Present failed: " << HrString(hr);
    NoteIfDeviceLost(hr);
    return false;
  }
  presents_->NotePresented(size);
  return true;
}

bool WinD3DSurface::NoteIfDeviceLost(HRESULT hr) {
  if (!IsDeviceLost(hr)) {
    return false;
  }
  MOMO_LOG(ERROR) << "Direct3D device lost (" << HrString(hr) << ").";
  lost_ = true;  // 下一次取帧时 FallbackSurface 回退软件
  return true;
}

}  // namespace momo::embedder::windows
