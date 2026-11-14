#pragma once

// Windows 平台集成工厂(docs/03 §2;对应 macOS 的 mac_platform_integration.h)。纯 C++ 头(不含 <windows.h>),
// 供入口(main_win.cc)使用。
//
// 必须在 OS 主线程调用, 且在此之前进程已声明 Per-Monitor-V2 DPI 感知(入口的
// WinAppDelegate::EnablePerMonitorDpiAwareness)。内部: 本线程 CoInitializeEx(STA, ShellExecuteW 需要)、
// 隐藏消息窗口实现的 NativeLoop(win_native_loop.h)、进程内唯一的指针输入(WinMouseInput)、显示器布局
// (WinDisplays)、应用对象(WinApp)、HWND 窗口(win_window.h)、DwmFlush vsync(win_vsync_dwm.h)、
// 剪贴板/打开 URL(win_services.h)。

#include <memory>

#include "embedder/common/platform_integration.h"

namespace momo::embedder::windows {

class WinAppObserver;

// observer: 应用级系统事件(前台状态)的接收方, 即入口的 WinAppDelegate(同 macOS 的 NSApp.delegate);
// 可为空, 非空时须比返回的对象活得久。
std::unique_ptr<PlatformIntegration> CreateWinPlatformIntegration(WinAppObserver* observer);

}  // namespace momo::embedder::windows
