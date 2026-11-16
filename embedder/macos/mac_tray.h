#pragma once

// MacTray: PlatformTray 的 AppKit 实现——菜单栏右侧的状态项(NSStatusItem)。仅被 embedder/macos 的 .mm 包含。
//  - 设置了上下文菜单: 菜单交给 NSStatusItem, 点击由系统打开菜单(不报告点击)。
//  - 没有菜单: 左键 / 右键抬起、双击报告给 TrayDelegate。
//  - 图片: 文件名(去掉 @2x 与扩展名)以 "Template" 结尾的是模板图像;旁边有 <name>@2x.<ext> 时作为高分屏表示。
// 线程约定: 仅 platform 线程(主线程)。

#import <Cocoa/Cocoa.h>

#include <optional>
#include <string>
#include <string_view>

#include "base/macros.h"
#include "embedder/common/platform_menu.h"

@class MomoTrayTarget;

namespace momo::embedder::macos {

class MacMenus;

class MacTray final : public PlatformTray {
 public:
  // menus 与 delegate 必须比本对象活得久。
  MacTray(MacMenus& menus, int64_t id, TrayDelegate* delegate);
  ~MacTray() override;  // 移除状态项

  Status SetImage(const std::string& path) override;
  void SetToolTip(std::string_view text) override;
  Status SetTitle(std::string_view title) override;
  void SetContextMenu(const MenuModel* menu) override;
  std::optional<Rect> bounds() const override;

  // 状态项按钮的 action(由 MomoTrayTarget 调用)。
  void OnButtonAction();

 private:
  MacMenus& menus_;
  const int64_t id_;
  TrayDelegate* const delegate_;
  NSStatusItem* item_;
  MomoTrayTarget* target_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(MacTray);
};

}  // namespace momo::embedder::macos
