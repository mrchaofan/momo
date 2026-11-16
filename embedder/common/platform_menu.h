#pragma once

// PlatformMenus: 原生菜单与托盘图标(docs/03 §2)。embedder 的平台抽象, engine 不可见;由宿主的 momo/menu、
// momo/tray 通道(menu_channel.h、tray_channel.h)调用。平台按 MenuModel 构建原生菜单, 不保存 JS 的任何状态:
// 内容变化 = 再设置一次。
//
// 菜单项被激活(点击, 或应用菜单的快捷键)时:
//   - 普通菜单项: MenuDelegate::OnMenuItemActivated(id)——不论菜单用在哪里(应用菜单、上下文菜单、托盘菜单)。
//   - role 菜单项: 平台有对应的系统标准动作时由系统执行(macOS: 沿响应链发送标准 action, 可用状态也由系统
//     判定), 不回调;没有时(Windows)回调 MenuDelegate::OnMenuRoleActivated, 由宿主公共代码执行。
// 线程约定: 仅 platform 线程调用;回调在 platform 线程。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/closure.h"
#include "base/geometry.h"
#include "base/status.h"
#include "embedder/common/menu_model.h"
#include "embedder/common/platform_window.h"

namespace momo::embedder {

class MenuDelegate {
 public:
  virtual void OnMenuItemActivated(int64_t item_id) = 0;
  virtual void OnMenuRoleActivated(MenuRole role) = 0;

 protected:
  ~MenuDelegate() = default;
};

enum class TrayClick { kClick, kRightClick, kDoubleClick };

class TrayDelegate {
 public:
  // 用户点击了托盘图标(设置了上下文菜单时, 打开菜单的那次点击不报告)。bounds: 图标的屏幕框。
  virtual void OnTrayClicked(int64_t tray_id, TrayClick click, const std::optional<Rect>& bounds) = 0;

 protected:
  ~TrayDelegate() = default;
};

// 一个托盘图标: 存在即显示, 析构即移除。
class PlatformTray {
 public:
  virtual ~PlatformTray() = default;
  // 图片文件的绝对路径(macOS: PNG, 文件名以 Template 结尾为模板图像, 旁边的 @2x 文件用于高分屏;
  // Windows: .ico)。
  virtual Status SetImage(const std::string& path) = 0;
  virtual void SetToolTip(std::string_view text) = 0;
  // 图标旁边的文字(只有 macOS)。
  virtual Status SetTitle(std::string_view title) {
    (void)title;
    return UnimplementedError("tray titles are not supported by this platform");
  }
  // 上下文菜单;nullptr 移除。
  virtual void SetContextMenu(const MenuModel* menu) = 0;
  // 图标在屏幕上的框(逻辑点, 主屏左上原点);取不到时为空。
  virtual std::optional<Rect> bounds() const = 0;
};

class PlatformMenus {
 public:
  virtual ~PlatformMenus() = default;
  // 菜单项激活的接收方(应用菜单、上下文菜单、托盘菜单共用);nullptr 解除。
  virtual void SetDelegate(MenuDelegate* delegate) = 0;
  // 应用菜单(macOS: 菜单栏;Windows: 每个窗口的菜单栏 + 快捷键)。nullptr: 平台的默认菜单。
  virtual void SetApplicationMenu(const MenuModel* menu) = 0;
  // 弹出上下文菜单(同时只有一个: 先关闭打开着的)。菜单由系统的跟踪循环(嵌套的原生循环)驱动, 所以不在本次
  // 调用里弹出: 调用立即返回, 平台随后在 platform 任务之外打开菜单——在任务里进入嵌套循环会让 platform
  // EventLoop 停转(macOS: GCD 主队列的任务里, 嵌套的 run loop 不再服务主队列)。
  // on_closed: 菜单关闭后在 platform 线程调用恰好一次(包括被 ClosePopup 或下一次 Popup 关闭、没能打开)。
  // position: window 内容区里的逻辑坐标;为空 = 鼠标指针处。window 可为空(只能在指针处弹出)。
  virtual void Popup(const MenuModel& menu, PlatformWindow* window, std::optional<Point> position,
                     MoveOnlyFunction<void()> on_closed) = 0;
  // 关闭打开着的上下文菜单(没有时什么也不做)。
  virtual void ClosePopup() = 0;
  // 创建托盘图标。delegate 必须比返回的对象活得久。
  virtual StatusOr<std::unique_ptr<PlatformTray>> CreateTray(int64_t tray_id, const std::string& image_path,
                                                             TrayDelegate* delegate) = 0;
};

}  // namespace momo::embedder
