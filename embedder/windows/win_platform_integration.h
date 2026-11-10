#pragma once

// Windows 平台集成工厂(docs/03 §2)。纯 C++ 头(不含 <windows.h>), 供 embedder/windows 使用。
// 注意: 实现(embedder/windows/*.cc)未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
// 必须在 OS 主线程调用, 且在此之前进程已声明 Per-Monitor-V2 DPI 感知(embedder 的
// WinAppDelegate::EnablePerMonitorDpiAwareness)。内部: 本线程 CoInitializeEx(STA, ShellExecuteW 需要)、
// 隐藏消息窗口实现的 NativeLoop(win_native_loop.h)、HWND 窗口(win_window.h)、DwmFlush vsync
// (win_vsync_dwm.h)、IMM32 输入法(win_text_input.h)、GDI 软件 surface(win_surface_software.h)、
// 剪贴板/打开 URL(win_services.h)。GPU(ANGLE)surface 未实现。

#include <memory>

#include "embedder/common/platform_integration.h"

namespace momo::embedder::windows {

std::unique_ptr<PlatformIntegration> CreateWinPlatformIntegration();

}  // namespace momo::embedder::windows
