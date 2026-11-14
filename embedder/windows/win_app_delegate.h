#pragma once

// WinAppDelegate: Win32 进程级集成(docs/01 §5), 把应用级系统事件翻译给 embedder::Application
// (对应 macOS 的 MacAppDelegate)。属于入口(momo_app_win), 不在平台实现库里。
//  - 前台状态: WinAppObserver::OnAppActiveChanged(WinApp 汇集各窗口的 WM_ACTIVATEAPP)→ Application::OnAppActiveChanged。
//  - 重新激活: 用户再次启动应用(开始菜单、任务栏、快捷方式)= macOS 点击 Dock 图标。后续实例经 WM_COPYDATA 把命令行
//    转发给首个实例并退出;首个实例把最前面的可见窗口带到前台(系统在 macOS 上做的事), 再调用
//    Application::OnReopen(JS 的 app 'activate' 事件, 例如没有窗口时重新打开一个)。
//  - 注销/关机: WM_QUERYENDSESSION 允许结束;WM_ENDSESSION 时有序关闭(Application::Terminate)——此后进程随时会被
//    系统结束, 无法异步询问 JS(macOS 的注销会走可取消的 Quit, Windows 的结束会话不等待应用)。
//  - 所有窗口关闭后应用不退出, 由 JS 决定(momo/app windowAllClosed)。Windows 没有应用级的"隐藏"。
//  - 进程 DPI 感知: 必须在任何窗口创建前调用 SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)
//    (exe 清单 momo_app_win.manifest 也声明了 PerMonitorV2;两者都在时 API 返回 ACCESS_DENIED, 忽略)。
//  - 单实例(app_id = exe 文件名: 打包后每个应用的 exe 不同, 互不干扰): 命名互斥量 "Local\momo.<app_id>" +
//    接收窗口(隐藏的顶层窗口, 类名 "MomoSingleInstance.<app_id>";顶层窗口才能收到 WM_QUERYENDSESSION /
//    WM_ENDSESSION 广播)。后续实例 AllowSetForegroundWindow 让首个实例
//    可以把自己的窗口带到前台。
//  - 命令行: UTF-16 → UTF-8;资源目录: exe 所在目录(docs/09 §4)。
// 消息循环本身由 embedder/windows 的 NativeLoop 提供;接收窗口属于主线程, 在该泵(及系统模态循环)中照常收到消息。
// 线程约定: 仅 OS 主线程。

#include <filesystem>
#include <string>
#include <vector>

#include "base/macros.h"
#include "embedder/windows/win_app.h"

namespace momo::embedder {

class Application;

class WinAppDelegate final : public windows::WinAppObserver {
 public:
  WinAppDelegate() = default;
  ~WinAppDelegate();

  // 非拥有;Application 由 main 持有, 在解绑(置空)之后才销毁。
  void set_application(Application* application) { application_ = application; }

  static void EnablePerMonitorDpiAwareness();

  // 首个实例: 持有互斥量并创建接收窗口, 返回 true。
  // 已有实例运行时: 把本进程命令行转发给它(由它重新激活), 返回 false。
  bool AcquireSingleInstance(const std::wstring& app_id);

  static std::vector<std::string> CommandLineArgsUtf8();  // 不含 argv[0]
  static std::filesystem::path ExecutablePath();
  static std::filesystem::path ExecutableDir() { return ExecutablePath().parent_path(); }

  // windows::WinAppObserver
  void OnAppActiveChanged(bool active) override;

 private:
  struct Receiver;  // 接收窗口的 WndProc(定义在 .cc)

  // 后续实例转发了命令行(参数目前只记录日志;打开文件等语义待定义)。
  void OnSecondInstance(std::vector<std::string> args);
  void OnEndSession();

  Application* application_ = nullptr;
  void* instance_mutex_ = nullptr;   // HANDLE
  void* receiver_window_ = nullptr;  // HWND

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinAppDelegate);
};

}  // namespace momo::embedder
