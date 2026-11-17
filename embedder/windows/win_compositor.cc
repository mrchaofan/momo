#include "embedder/windows/win_compositor.h"

#include <string>

namespace momo::embedder::windows {

StatusOr<std::shared_ptr<WinCompositor>> WinCompositor::Create(HWND hwnd) {
  std::shared_ptr<WinCompositor> compositor(new WinCompositor());
  // 不绑定渲染设备: 内容只来自交换链(D3D12 队列创建)与扩展自己的 visual(WebView2)。
  HRESULT hr = ::DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&compositor->device_));
  if (SUCCEEDED(hr)) hr = compositor->device_->CreateTargetForHwnd(hwnd, FALSE, &compositor->target_);
  if (SUCCEEDED(hr)) hr = compositor->device_->CreateVisual(&compositor->root_);
  if (SUCCEEDED(hr)) hr = compositor->target_->SetRoot(compositor->root_.Get());
  if (SUCCEEDED(hr)) hr = compositor->device_->Commit();
  if (FAILED(hr)) {
    return UnavailableError("DirectComposition unavailable: HRESULT " + std::to_string(static_cast<long>(hr)));
  }
  return compositor;
}

void WinCompositor::Update(const std::function<void()>& change) {
  MutexLock lock(mutex_);
  change();
  device_->Commit();
}

void WinCompositor::Detach() {
  Update([this] { target_->SetRoot(nullptr); });
}

}  // namespace momo::embedder::windows
