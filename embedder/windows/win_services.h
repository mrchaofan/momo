#pragma once

// WinServices: PlatformServices 的 Win32 实现(剪贴板 CF_UNICODETEXT、OpenUrl = ShellExecuteW)。
// 仅被 embedder/windows 的 .cc 包含。
// 注意: 本文件未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//
//  - 剪贴板: UTF-8 ↔ UTF-16;写入时 "\n" → "\r\n"(Windows 文本约定), 读取时 "\r\n" → "\n"(与 mac 一致)。
//    OpenClipboard 需要一个属于本线程的窗口作为 owner(NULL owner 时 SetClipboardData 会失败):
//    使用 WinNativeLoop 的消息窗口。剪贴板被其他进程占用时短暂重试(最多 ~20ms)。
//  - OpenUrl: 只接受带 scheme 的 URL(至少两个字符, 排除 "C:\..." 这类路径), 拒绝 file:(避免借此
//    执行本地程序);交给 ShellExecuteW("open")。调用线程需已 CoInitializeEx(由 WinPlatformIntegration 负责)。
// 线程约定: 仅 platform 线程。

#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_util.h"
#include "embedder/common/platform_services.h"

namespace momo::embedder::windows {

class WinServices final : public PlatformServices {
 public:
  explicit WinServices(HWND clipboard_owner) : clipboard_owner_(clipboard_owner) {}

  StatusOr<std::string> GetClipboardText() override;
  Status SetClipboardText(std::string_view utf8) override;
  Status OpenUrl(std::string_view url) override;

 private:
  bool OpenClipboardWithRetry();

  HWND clipboard_owner_;
};

}  // namespace momo::embedder::windows
