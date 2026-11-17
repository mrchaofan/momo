#include "embedder/windows/win_surface_d3d.h"

#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "base/logging.h"
#include "embedder/windows/win_compositor.h"
#include "embedder/windows/win_platform_views.h"

namespace momo::embedder::windows {

using Microsoft::WRL::ComPtr;

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
// SwapChain: 一条组合交换链和以它为内容的 visual(主帧与每个 overlay 各一条)。raster 线程使用。

class WinD3DSurface::SwapChain {
 public:
  static StatusOr<std::unique_ptr<SwapChain>> Create(IDXGIFactory4* factory, ID3D12CommandQueue* queue,
                                                     IDCompositionDesktopDevice* composition, DXGI_ALPHA_MODE alpha,
                                                     int width, int height) {
    std::unique_ptr<SwapChain> chain(new SwapChain());
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(std::max(1, width));
    desc.Height = static_cast<UINT>(std::max(1, height));
    desc.Format = kFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;  // 组合交换链只支持 STRETCH(内容按缓冲尺寸显示, visual 不缩放它)
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = alpha;
    ComPtr<IDXGISwapChain1> chain1;
    HRESULT hr = factory->CreateSwapChainForComposition(queue, &desc, nullptr, &chain1);
    if (SUCCEEDED(hr)) hr = chain1.As(&chain->chain_);
    if (SUCCEEDED(hr)) hr = composition->CreateVisual(&chain->visual_);
    if (SUCCEEDED(hr)) hr = chain->visual_->SetContent(chain->chain_.Get());
    if (FAILED(hr)) {
      return UnavailableError("CreateSwapChainForComposition failed: " + HrString(hr));
    }
    MOMO_RETURN_IF_ERROR(chain->GetBuffers());
    chain->width_ = static_cast<int>(desc.Width);
    chain->height_ = static_cast<int>(desc.Height);
    return chain;
  }

  IDCompositionVisual2* visual() const { return visual_.Get(); }
  bool SizeIs(int width, int height) const { return width == width_ && height == height_; }

  // 调用方保证 GPU 上没有使用后台缓冲的工作(ResizeBuffers 的前提)。返回失败时的 HRESULT 供判断设备丢失。
  HRESULT Resize(int width, int height) {
    ReleaseBuffers();
    width_ = height_ = 0;
    HRESULT hr = chain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr) && !GetBuffers().ok()) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
      width_ = width;
      height_ = height;
    }
    return hr;
  }

  ID3D12Resource* current_buffer() const {
    const UINT index = chain_->GetCurrentBackBufferIndex();
    return index < kBufferCount ? buffers_[index].Get() : nullptr;
  }

  HRESULT Present() { return chain_->Present(1, 0); }

 private:
  SwapChain() = default;

  Status GetBuffers() {
    for (UINT i = 0; i < kBufferCount; ++i) {
      if (FAILED(chain_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])))) {
        ReleaseBuffers();
        return InternalError("IDXGISwapChain::GetBuffer failed");
      }
    }
    return Status::Ok();
  }
  void ReleaseBuffers() {
    for (ComPtr<ID3D12Resource>& buffer : buffers_) buffer.Reset();
  }

  ComPtr<IDXGISwapChain3> chain_;
  ComPtr<ID3D12Resource> buffers_[kBufferCount];
  ComPtr<IDCompositionVisual2> visual_;
  int width_ = 0;
  int height_ = 0;
};

// ---------------------------------------------------------------------------
// WinD3DSurface

StatusOr<std::unique_ptr<WinD3DSurface>> WinD3DSurface::Create(HWND hwnd, std::shared_ptr<const WinD3DDevice> device,
                                                               std::shared_ptr<WinCompositor> compositor,
                                                               std::shared_ptr<PresentQueue> presents) {
  std::unique_ptr<WinD3DSurface> surface(
      new WinD3DSurface(hwnd, std::move(device), std::move(compositor), std::move(presents)));
  MOMO_RETURN_IF_ERROR(surface->Initialize());
  return surface;
}

WinD3DSurface::WinD3DSurface(HWND hwnd, std::shared_ptr<const WinD3DDevice> device,
                             std::shared_ptr<WinCompositor> compositor, std::shared_ptr<PresentQueue> presents)
    : hwnd_(hwnd), device_(std::move(device)), compositor_(std::move(compositor)), presents_(std::move(presents)) {}

Status WinD3DSurface::Initialize() {
  if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) {
    return UnavailableError("CreateDXGIFactory2 failed");
  }
  const HRESULT hr = device_->device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
  fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(hr) || fence_event_ == nullptr) {
    return UnavailableError("CreateFence failed: " + HrString(hr));
  }
  RECT client{};
  ::GetClientRect(hwnd_, &client);
  MOMO_ASSIGN_OR_RETURN(base_, SwapChain::Create(factory_.Get(), device_->queue(), compositor_->device(),
                                                 DXGI_ALPHA_MODE_IGNORE, client.right - client.left,
                                                 client.bottom - client.top));
  compositor_->Update([this] { compositor_->root()->AddVisual(base_->visual(), FALSE, nullptr); });
  return Status::Ok();
}

WinD3DSurface::~WinD3DSurface() {
  // raster 已解绑(Shell::DetachView 先于窗口销毁): 本线程独占交换链。设备丢失后不等待(fence 不再推进)。
  if (base_ != nullptr && !device_lost()) {
    WaitForQueueIdle();
  }
  overlays_.clear();
  base_.reset();
  if (fence_event_ != nullptr) {
    ::CloseHandle(fence_event_);
  }
}

platform::GpuContextInfo WinD3DSurface::gpu_context() const { return {device_->device(), device_->queue()}; }

bool WinD3DSurface::device_lost() const {
  return lost_ || device_->device()->GetDeviceRemovedReason() != S_OK;
}

bool WinD3DSurface::SupportsPlatformViews() const { return platform_views_ != nullptr && base_ != nullptr; }

void WinD3DSurface::Deactivate() {
  if (!device_lost()) {
    WaitForQueueIdle();  // 丢失的设备上 fence 不会再推进: 不等待
  }
  overlays_.clear();
  base_.reset();
  compositor_->Detach();  // 之后窗口内容回到 GDI(软件 surface)
  if (platform_views_ != nullptr) {
    platform_views_->Disable();  // 软件路径不支持平台视图
  }
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

StatusOr<platform::SurfaceFrame> WinD3DSurface::FrameOf(SwapChain& chain, Size size) {
  const int width = static_cast<int>(size.width);
  const int height = static_cast<int>(size.height);
  if (width <= 0 || height <= 0) {
    return FailedPreconditionError("empty surface size");
  }
  if (!chain.SizeIs(width, height)) {
    // ResizeBuffers 要求没有任何对后台缓冲的引用、GPU 上也没有使用它们的工作(后端每帧已等待完成)。
    WaitForQueueIdle();
    if (const HRESULT hr = chain.Resize(width, height); FAILED(hr)) {
      NoteIfDeviceLost(hr);
      return InternalError("IDXGISwapChain::ResizeBuffers failed: " + HrString(hr));
    }
  }
  ID3D12Resource* buffer = chain.current_buffer();
  if (buffer == nullptr) {
    return InternalError("invalid back buffer index");
  }
  platform::SurfaceFrame frame;
  frame.kind = platform::SurfaceKind::kDirect3D;
  frame.gpu_target = buffer;
  frame.size = {static_cast<double>(width), static_cast<double>(height)};
  return frame;
}

StatusOr<platform::SurfaceFrame> WinD3DSurface::AcquireFrame(Size physical_size) {
  if (base_ == nullptr) {
    return FailedPreconditionError("swap chain released");
  }
  MOMO_ASSIGN_OR_RETURN(platform::SurfaceFrame frame, FrameOf(*base_, physical_size));
  frame.present = [this, size = frame.size] {
    if (!Present(*base_)) {
      return false;
    }
    ReportPresented(size);
    return true;
  };
  return frame;
}

StatusOr<platform::CompositionFrame> WinD3DSurface::AcquireCompositionFrame(
    Size physical_size, const platform::CompositionLayout& layout) {
  if (!SupportsPlatformViews()) {
    return UnimplementedError("platform views are not available on this surface");
  }
  platform::CompositionFrame frame;
  MOMO_ASSIGN_OR_RETURN(frame.base, FrameOf(*base_, physical_size));
  // overlay: 每个非空分片一条透明交换链, 尺寸 = 分片范围。
  if (overlays_.size() < layout.overlays.size()) {
    overlays_.resize(layout.overlays.size());
  }
  frame.overlays.resize(layout.overlays.size());
  for (size_t i = 0; i < layout.overlays.size(); ++i) {
    if (!layout.overlays[i]) {
      continue;
    }
    const Size size = layout.overlays[i]->size();
    if (overlays_[i] == nullptr) {
      StatusOr<std::unique_ptr<SwapChain>> chain =
          SwapChain::Create(factory_.Get(), device_->queue(), compositor_->device(), DXGI_ALPHA_MODE_PREMULTIPLIED,
                            static_cast<int>(size.width), static_cast<int>(size.height));
      if (!chain.ok()) {
        MOMO_LOG(WARNING) << "Overlay " << i << " dropped: " << chain.status();
        continue;
      }
      overlays_[i] = std::move(chain).value();
    }
    StatusOr<platform::SurfaceFrame> overlay = FrameOf(*overlays_[i], size);
    if (!overlay.ok()) {
      MOMO_LOG(WARNING) << "Overlay " << i << " dropped: " << overlay.status();
      continue;
    }
    frame.overlays[i] = *overlay;
  }
  // 拷贝布局里实际取到帧的 overlay(present 时只呈现并排列这些)。
  platform::CompositionLayout presented = layout;
  for (size_t i = 0; i < presented.overlays.size(); ++i) {
    if (!frame.overlays[i]) {
      presented.overlays[i].reset();
    }
  }
  frame.present = [this, size = frame.base.size, presented = std::move(presented)] {
    return PresentComposition(size, presented);
  };
  return frame;
}

bool WinD3DSurface::PresentComposition(Size size, const platform::CompositionLayout& layout) {
  bool ok = Present(*base_);
  std::vector<IDCompositionVisual2*> overlay_visuals(layout.overlays.size(), nullptr);
  for (size_t i = 0; i < layout.overlays.size(); ++i) {
    if (layout.overlays[i] && Present(*overlays_[i])) {
      overlay_visuals[i] = overlays_[i]->visual();
    }
  }
  if (!ok) {
    return false;
  }
  // z 序与视图几何在一次提交里生效, 与上面的 Present 落在同一个 DWM 帧。
  compositor_->Update([&] { platform_views_->Compose(layout, base_->visual(), overlay_visuals); });
  // platform 线程: 视图挂入、尺寸、焦点、命中几何(入队的呈现只由窗口执行, 窗口销毁后随队列丢弃)。
  presents_->Post(size, [views = platform_views_, layout] { views->OnPresented(layout); });
  return true;
}

bool WinD3DSurface::Present(SwapChain& chain) {
  const HRESULT hr = chain.Present();
  if (FAILED(hr)) {
    MOMO_LOG(ERROR) << "IDXGISwapChain::Present failed: " << HrString(hr);
    NoteIfDeviceLost(hr);
    return false;
  }
  return true;
}

void WinD3DSurface::ReportPresented(Size size) {
  if (presents_->pending() > 0) {
    presents_->Post(size, [] {});
  } else {
    presents_->NotePresented(size);
  }
}

void WinD3DSurface::WaitForQueueIdle() {
  if (fence_ == nullptr || fence_event_ == nullptr) {
    return;
  }
  const uint64_t value = ++fence_value_;
  if (FAILED(device_->queue()->Signal(fence_.Get(), value))) {
    return;
  }
  if (fence_->GetCompletedValue() < value && SUCCEEDED(fence_->SetEventOnCompletion(value, fence_event_))) {
    ::WaitForSingleObject(fence_event_, INFINITE);
  }
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
