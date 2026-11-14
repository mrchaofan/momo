#pragma once

// WinCaptionButtons: 隐藏标题栏时由宿主绘制的系统窗口控制按钮(最小化 / 最大化·还原 / 关闭)。
// 仅被 embedder/windows 的 .cc 包含。
//
// macOS 隐藏标题栏后红绿灯仍由系统绘制并浮在内容之上;Windows 去掉标题栏(WM_NCCALCSIZE)后系统不再画按钮,
// 所以由宿主画——应用侧看到的行为一致: 按钮浮在内容之上, 占用区域经 WindowControlsOverlay 报告, 应用只需避让。
//  - 外观: Windows 10/11 标准样式(46 点宽, Segoe Fluent Icons / Segoe MDL2 Assets 字形), 悬停/按下底色,
//    关闭按钮悬停为红色;跟随深色模式与窗口激活状态;不可缩放时最大化按钮停用。
//  - 实现: 分层子窗口(WS_EX_LAYERED, Win8+ 支持子窗口分层)+ UpdateLayeredWindow 逐像素 alpha:
//    按钮之外透明, 应用自绘的标题栏背景透出来。不接收任何鼠标消息(HTTRANSPARENT + WS_EX_TRANSPARENT):
//    命中、悬停、点击都由父窗口的非客户区消息处理(WM_NCHITTEST 返回 HTMINBUTTON / HTMAXBUTTON / HTCLOSE,
//    Windows 11 据 HTMAXBUTTON 弹出贴靠布局)。
// 线程约定: 仅 platform 线程。生命周期: 由 WinWindow 拥有, 父窗口比本对象活得久。

#include "embedder/windows/win_frame.h"
#include "embedder/windows/win_util.h"

namespace momo::embedder::windows {

struct WinCaptionButtonsAppearance {
  bool visible = false;
  WinCaptionButtonsLayout layout;   // 内容区坐标, 逻辑点
  double dpr = 1.0;
  bool maximized = false;           // 最大化按钮显示为"还原"
  bool active = true;               // 窗口激活(非激活时字形变淡)
  bool dark = false;                // 深色模式
  WinFrameHit hover = WinFrameHit::kClient;
  WinFrameHit pressed = WinFrameHit::kClient;
  friend bool operator==(const WinCaptionButtonsAppearance&, const WinCaptionButtonsAppearance&) = default;
};

class WinCaptionButtons {
 public:
  explicit WinCaptionButtons(HWND parent);
  ~WinCaptionButtons();
  WinCaptionButtons(const WinCaptionButtons&) = delete;
  WinCaptionButtons& operator=(const WinCaptionButtons&) = delete;

  // 外观有变化时重画(并按布局移动子窗口)。
  void Update(const WinCaptionButtonsAppearance& appearance);
  HWND hwnd() const { return hwnd_; }

 private:
  void Paint();

  HWND parent_;
  HWND hwnd_ = nullptr;
  WinCaptionButtonsAppearance appearance_;
  bool painted_ = false;
};

}  // namespace momo::embedder::windows
