#pragma once

// WinAppDelegate: Win32 进程级集成(docs/01 §5)。
// 注意: 本文件(及 .cc)未在 Windows 上编译验证(作者环境为 macOS, 无 Windows SDK)。
//  - 进程 DPI 感知: 必须在任何窗口创建前调用 SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)
//    (exe 清单 momo_app_win.manifest 也声明了 PerMonitorV2;两者都在时 API 返回 ACCESS_DENIED, 忽略)。
//  - 单实例: 命名互斥量 "Local\momo.<app_id>" + 消息窗口(HWND_MESSAGE, 类名 "MomoSingleInstance.<app_id>")。
//    后续实例经 WM_COPYDATA 把完整命令行(UTF-16)转发给首个实例, 并 AllowSetForegroundWindow 让其
//    可以把自己的窗口带到前台;首个实例在 platform 线程(消息循环中)回调 SecondInstanceHandler。
//  - 命令行: UTF-16 → UTF-8
//  - 资源目录: exe 所在目录(docs/09 §4)
// 消息循环本身由 embedder/windows 的 NativeLoop 提供(自有消息泵 + 隐藏消息窗口);
// 接收窗口属于主线程, 因此在该泵(及系统模态循环)中照常收到 WM_COPYDATA。
// 线程约定: 仅 OS 主线程。

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "base/macros.h"

namespace momo::embedder {

class WinAppDelegate {
 public:
  // args 不含 argv[0]。
  using SecondInstanceHandler = std::function<void(std::vector<std::string> args)>;

  WinAppDelegate() = default;
  ~WinAppDelegate();

  static void EnablePerMonitorDpiAwareness();

  // 首个实例: 持有互斥量并创建接收窗口, 返回 true。
  // 已有实例运行时: 把本进程命令行转发给它(由它把主窗口带到前台), 返回 false。
  bool AcquireSingleInstance(const std::wstring& app_id);

  // 首个实例收到后续实例的命令行时调用(在 WM_COPYDATA 处理中同步调用: 应尽快返回)。可传空。
  void SetSecondInstanceHandler(SecondInstanceHandler handler) {
    second_instance_handler_ = std::move(handler);
  }

  // 把窗口(HWND)带到前台;最小化时先还原。
  static void ActivateWindow(void* hwnd);

  static std::vector<std::string> CommandLineArgsUtf8();  // 不含 argv[0]
  static std::filesystem::path ExecutableDir();

 private:
  struct Receiver;  // 接收窗口的 WndProc(定义在 .cc)

  void* instance_mutex_ = nullptr;  // HANDLE
  void* receiver_window_ = nullptr;  // HWND
  SecondInstanceHandler second_instance_handler_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinAppDelegate);
};

}  // namespace momo::embedder
