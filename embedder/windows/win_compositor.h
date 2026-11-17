#pragma once

// WinCompositor: 一个窗口的 DirectComposition 树(ARCHITECTURE.md §3.13)。仅被 embedder/windows 的 .cc 包含。
//
// 窗口内容区的 GPU 画面都是这棵树上的 visual(自下而上):
//   root
//   ├── 主帧(WinD3DSurface 的交换链)
//   ├── 平台视图 0 的容器(WinPlatformViews: 位置、裁剪、透明度;其下是视图自己的内容, 如 WebView2)
//   ├── overlay 0(WinD3DSurface 的透明交换链: 绘制顺序在视图 0 之后的 momo 内容)
//   └── …
// 树挂在 HWND 上(目标不置顶: 窗口的子窗口——宿主绘制的控制按钮——在它之上)。软件回退后整棵树摘下, 窗口内容回到
// GDI(WinSoftwareSurface)。
//
// 线程: DirectComposition 线程安全, 但一个设备只有一批待提交的修改: raster 线程(每帧的几何与交换链)和 platform
// 线程(平台视图挂入、扩展请求提交)都经 Update 修改并提交, 互斥保证每次提交都是完整的一批。
// 生命周期: 窗口创建(shared_ptr), surface 与平台视图宿主共享;窗口销毁时最后释放。

#include <dcomp.h>
#include <wrl/client.h>

#include <functional>
#include <memory>

#include "base/lock.h"
#include "base/macros.h"
#include "base/status.h"
#include "embedder/windows/win_util.h"

namespace momo::embedder::windows {

class WinCompositor final {
 public:
  static StatusOr<std::shared_ptr<WinCompositor>> Create(HWND hwnd);

  IDCompositionDesktopDevice* device() const { return device_.Get(); }
  IDCompositionVisual2* root() const { return root_.Get(); }

  // 任意线程: 在锁内执行 change(修改树), 然后提交。change 不得再调用 Update。
  void Update(const std::function<void()>& change);
  // 摘下整棵树(之后窗口内容由 GDI 绘制)。
  void Detach();

 private:
  WinCompositor() = default;

  Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> device_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
  Microsoft::WRL::ComPtr<IDCompositionVisual2> root_;
  Mutex mutex_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinCompositor);
};

}  // namespace momo::embedder::windows
