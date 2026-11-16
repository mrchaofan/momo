#pragma once

// MenuChannel: 宿主的菜单通道 momo/menu(docs/03 §2)。框架的 Menu 对象持有菜单内容与点击回调;这里把 JS 发来的
// 描述(MenuModel)交给平台(PlatformMenus), 把菜单项的激活送回 JS。
//
// momo/menu(StandardMethodCodec;菜单项的格式见 menu_model.h 的 ParseMenuModel):
//   JS → 宿主:
//     setApplicationMenu {items: [item] | null}     应用菜单;null = 平台的默认菜单
//     popup {id, items: [item], view?, x?, y?} → true   上下文菜单(同时只有一个)。回复 = 已交给系统弹出;
//                                                   关闭时另发 popupClosed。x, y: 视图内容区里的逻辑坐标
//                                                   (缺省为鼠标指针处)
//     closePopup                                    关闭打开着的上下文菜单
//   宿主 → JS:
//     popupClosed {id}                              上下文菜单已关闭(点击了菜单项时, itemClicked 在它之前)
//     itemClicked {id}                              菜单项被点击或其快捷键被按下(应用菜单、上下文菜单、托盘菜单
//                                                   共用;role 菜单项由宿主执行, 不报告)
// 平台没有菜单(PlatformIntegration::menus() 为空)时各方法回复 "unimplemented"。
// 线程约定: 归属 platform 线程;处理器在 platform 线程运行。

#include <memory>

#include "base/macros.h"
#include "base/status.h"
#include "base/task_runner.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/host_channel.h"
#include "embedder/common/platform_menu.h"
#include "engine/common/message_channel_hub.h"
#include "engine/platform/view.h"

namespace momo::embedder {

class MenuChannel final : private MenuDelegate {
 public:
  // 应用(Application)。platform 线程回调。
  class Host {
   public:
    // 上下文菜单所在的窗口(未知视图为空)。
    virtual PlatformWindow* FindWindow(platform::ViewId view) = 0;
    // 执行一个 role(平台没有对应的系统标准动作时, 见 platform_menu.h)。
    virtual void PerformMenuRole(MenuRole role) = 0;

   protected:
    ~Host() = default;
  };

  static constexpr const char kName[] = "momo/menu";

  // menus 可为空;非空时与 host 都必须比本对象活得久。
  MenuChannel(std::shared_ptr<engine::MessageChannelHub> hub, std::shared_ptr<TaskRunner> platform_runner,
              PlatformMenus* menus, Host* host);
  ~MenuChannel();  // 解除 MenuDelegate, 关闭打开着的上下文菜单

  // 处理一次 JS 调用(测试可直接调用)。
  StatusOr<engine::EncodableValue> HandleCall(const engine::MethodCall& call) { return channel_.Dispatch(call); }

 private:
  // MenuDelegate
  void OnMenuItemActivated(int64_t item_id) override;
  void OnMenuRoleActivated(MenuRole role) override;

  PlatformMenus* const menus_;
  Host* const host_;
  HostChannel channel_;
  // 上下文菜单的关闭回调可能晚于本对象析构: 以它判活。
  const std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  MOMO_DISALLOW_COPY_AND_ASSIGN(MenuChannel);
};

}  // namespace momo::embedder
