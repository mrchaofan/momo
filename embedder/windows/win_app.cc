#include "embedder/windows/win_app.h"

#include "embedder/windows/win_util.h"
#include "embedder/windows/win_window.h"

namespace momo::embedder::windows {

Status WinApp::Activate(bool steal) {
  HWND front = WinWindow::FrontmostVisibleHwnd();
  if (front == nullptr) {
    return Status::Ok();  // 没有可见窗口: 与 macOS 一致, 激活没有可见效果
  }
  if (!steal) {
    // 不抢占前台: 闪烁任务栏按钮直到窗口被用户激活(同 macOS 的 Dock 图标跳动)。
    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = front;
    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    ::FlashWindowEx(&flash);
    return Status::Ok();
  }
  if (::IsIconic(front)) {
    ::ShowWindow(front, SW_RESTORE);
  }
  // 前台锁: 只有当前拥有前台的进程(或被它 AllowSetForegroundWindow 的进程)才能成功;失败时退回请求关注。
  if (!::SetForegroundWindow(front)) {
    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = front;
    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    ::FlashWindowEx(&flash);
  }
  return Status::Ok();
}

void WinApp::OnActivateApp(bool active) {
  if (active_ == active) {
    return;
  }
  active_ = active;
  if (observer_ != nullptr) {
    observer_->OnAppActiveChanged(active);
  }
}

}  // namespace momo::embedder::windows
