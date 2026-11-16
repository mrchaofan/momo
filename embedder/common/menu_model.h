#pragma once

// 菜单的描述(momo/menu、momo/tray 通道)。embedder 的平台抽象, engine 不可见。
// 菜单的内容与点击回调归 JS(框架的 Menu / MenuItem);宿主只拿到这份不可变的描述, 平台层据此构建原生菜单
// (NSMenu / HMENU), 点击时以菜单项 id 报告(PlatformMenus 的 MenuDelegate)。内容变化 = JS 重新发送整份描述。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/status.h"
#include "engine/common/standard_codec.h"

namespace momo::embedder {

enum class MenuItemType { kNormal, kSeparator, kSubmenu, kCheckbox, kRadio };

// 由宿主执行的标准动作(JS 的 MenuItemRole)。带 role 的菜单项不向 JS 报告点击;标题与快捷键缺省由平台给出。
// 平台没有的 role, 菜单项不显示。
enum class MenuRole {
  kNone,
  // 应用
  kAbout, kServices, kHide, kHideOthers, kUnhide, kQuit,
  // 编辑(作用于获得焦点的内容)
  kUndo, kRedo, kCut, kCopy, kPaste, kSelectAll,
  // 窗口(作用于获得焦点的窗口)
  kMinimize, kZoom, kClose, kFront,
  // 整个子菜单
  kAppMenu, kEditMenu, kWindowMenu,
};

// 快捷键。key: 单个小写字符("n"、"1"、"+"), 或具名键("F5"、"Enter"、"Tab"、"Space"、"Backspace"、"Delete"、
// "Insert"、"Escape"、"Up"、"Down"、"Left"、"Right"、"Home"、"End"、"PageUp"、"PageDown");
// modifiers: engine::platform::kModifierShift | Control | Alt | Meta。字符串由框架解析(accelerator.ts)。
struct MenuAccelerator {
  std::string key;
  uint32_t modifiers = 0;
  friend bool operator==(const MenuAccelerator&, const MenuAccelerator&) = default;
};

struct MenuItem {
  int64_t id = 0;  // JS 侧的编号(进程内唯一), 点击时原样报告
  MenuItemType type = MenuItemType::kNormal;
  MenuRole role = MenuRole::kNone;
  std::string label;  // UTF-8;为空且有 role 时用平台的标准标题
  std::optional<MenuAccelerator> accelerator;
  bool enabled = true;
  bool checked = false;  // kCheckbox / kRadio
  std::vector<MenuItem> submenu;  // kSubmenu
};

using MenuModel = std::vector<MenuItem>;

// 解析 JS 发来的菜单项列表: [{id, type, label, role?, accelerator?: {key, modifiers}, enabled, checked,
// submenu?: [...]}]。类型或 role 不认识时 InvalidArgument。
StatusOr<MenuModel> ParseMenuModel(const engine::EncodableValue& items);

}  // namespace momo::embedder
