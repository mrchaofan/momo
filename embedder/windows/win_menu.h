#pragma once

// WinMenus: PlatformMenus 的 Win32 实现(对应 macOS 的 MacMenus / MacTray)。仅被 embedder/windows 的 .cc 包含。
// **未在 Windows 上编译与运行验证。**
//
//  - 应用菜单 = 每个 momo 窗口的菜单栏(SetMenu)。隐藏了标题栏的窗口没有菜单栏(内容铺满窗口), 快捷键照常生效。
//    没有设置应用菜单时窗口没有菜单栏。内容变化 = 给每个窗口换一个新的 HMENU(WinWindow::UpdateMenuBar)。
//  - 快捷键: Windows 的菜单不识别快捷键, 由本类匹配——WinWindow 在按键(WM_KEYDOWN / WM_SYSKEYDOWN)交给应用
//    内容之前调用 HandleKeyDown, 命中则与点击菜单项相同, 按键不再上报。编辑类 role(剪切/复制/粘贴/全选)的
//    快捷键只显示、不拦截: 这些按键归获得焦点的内容(框架的文本框自己处理)。
//  - 上下文菜单 = TrackPopupMenuEx, 在宿主窗口的消息处理里弹出(不在 platform EventLoop 的任务里, 见
//    platform_menu.h)。
//  - role: Windows 没有与 AppKit 标准 action 对应的机制, 激活时回调 MenuDelegate::OnMenuRoleActivated, 由宿主
//    公共代码执行(Application::PerformMenuRole)。只支持 quit、cut、copy、paste、selectAll、minimize、zoom、close
//    以及 editMenu、windowMenu 两个子菜单;其余 role(macOS 特有)的菜单项不显示。
//  - 托盘图标 = Shell_NotifyIcon(NOTIFYICON_VERSION_4), 回调消息发往宿主窗口。图片只支持 .ico。左键点击、
//    双击报告给 TrayDelegate;右键(或键盘菜单键)打开上下文菜单, 没有菜单时报告右键点击。资源管理器重启
//    (TaskbarCreated)后重新添加图标。
//  - 宿主窗口: 一个隐藏的顶层窗口(不是 message-only 窗口: 要能成为前台窗口托盘菜单才会在点击别处时关闭,
//    也要能收到 TaskbarCreated 广播)。
// 线程约定: 仅 platform(主)线程。

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/macros.h"
#include "embedder/common/platform_menu.h"
#include "embedder/windows/win_util.h"

#include <shellapi.h>

namespace momo::embedder::windows {

class WinDisplays;

class WinMenus final : public PlatformMenus {
 public:
  // displays 必须比本对象活得久。
  explicit WinMenus(WinDisplays& displays);
  ~WinMenus() override;

  // PlatformMenus
  void SetDelegate(MenuDelegate* delegate) override { delegate_ = delegate; }
  void SetApplicationMenu(const MenuModel* menu) override;
  void Popup(const MenuModel& menu, PlatformWindow* window, std::optional<Point> position,
             MoveOnlyFunction<void()> on_closed) override;
  void ClosePopup() override;
  StatusOr<std::unique_ptr<PlatformTray>> CreateTray(int64_t tray_id, const std::string& image_path,
                                                     TrayDelegate* delegate) override;

  // ---- 供 WinWindow 使用 ----
  // 新建一份应用菜单的菜单栏(调用方交给 SetMenu, 此后归窗口所有);没有应用菜单时为空。
  HMENU CreateMenuBar() const;
  // 窗口菜单栏的 WM_COMMAND(command 为菜单项的命令号)。
  void OnMenuBarCommand(UINT command);
  // 按键是否命中应用菜单的快捷键。命中即激活菜单项(repeat: 按住产生的重复, 命中但不再激活)。
  // modifiers: platform::kModifier*。
  bool HandleKeyDown(UINT virtual_key, uint32_t modifiers, bool repeat);

 private:
  // 一个菜单项被激活时要做的事: role 非空 → OnMenuRoleActivated, 否则 OnMenuItemActivated(id)。
  struct Command {
    int64_t id = 0;
    MenuRole role = MenuRole::kNone;
  };
  // 构建好的菜单: 菜单项的命令号是 commands 的下标 + 1。
  struct BuiltMenu {
    HMENU menu = nullptr;
    std::vector<Command> commands;
  };
  struct Accelerator {
    UINT virtual_key = 0;
    uint32_t modifiers = 0;
    Command command;
  };
  struct PendingPopup {
    BuiltMenu built;
    HWND owner = nullptr;
    POINT at{};
    MoveOnlyFunction<void()> on_closed;
  };
  class Tray;

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  // 向 menu 追加 model 的各项(递归子菜单);命令记入 commands, 快捷键记入 accelerators(可为空)。
  static void AppendItems(HMENU menu, const MenuModel& model, std::vector<Command>& commands,
                          std::vector<Accelerator>* accelerators);
  static BuiltMenu BuildPopupMenu(const MenuModel& model);
  void Activate(const Command& command);
  // 在 at(屏幕物理像素)弹出 built(阻塞到菜单关闭), 激活选中的菜单项, 然后销毁菜单。
  void TrackPopup(BuiltMenu built, HWND owner, POINT at);
  void ShowPendingPopup();
  Rect ToLogicalRect(const RECT& physical) const;

  WinDisplays& displays_;
  MenuDelegate* delegate_ = nullptr;
  HWND host_ = nullptr;
  UINT taskbar_created_message_ = 0;

  // 应用菜单
  std::optional<MenuModel> application_menu_;
  std::vector<Command> application_commands_;  // 菜单栏的命令号 → 命令
  std::vector<Accelerator> accelerators_;

  // 上下文菜单
  std::optional<PendingPopup> pending_popup_;  // 已请求、尚未弹出
  bool popup_open_ = false;

  std::map<UINT, Tray*> trays_;  // 存活的图标(图标 id → 对象;非拥有)

  MOMO_DISALLOW_COPY_AND_ASSIGN(WinMenus);
};

}  // namespace momo::embedder::windows
