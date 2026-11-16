#pragma once

// TrayChannel: 宿主的托盘通道 momo/tray(docs/03 §2)。拥有全部托盘图标(id → PlatformTray);框架里每个图标是
// 一个 Tray 对象, id 由 JS 分配。托盘菜单的点击与其他菜单一样经 momo/menu itemClicked 报告。
//
// momo/tray(StandardMethodCodec;id 为托盘 id):
//   JS → 宿主:
//     create {id, image}              image: 图片文件的绝对路径
//     destroy {id}
//     setImage {id, image} / setToolTip {id, text} / setTitle {id, title}(只有 macOS)
//     setContextMenu {id, items: [item] | null}
//     getBounds {id} → [x, y, w, h] | null      图标的屏幕框
//   宿主 → JS:
//     click / rightClick / doubleClick {id, bounds: [x, y, w, h] | null}
// 平台没有托盘(PlatformIntegration::menus() 为空)时各方法回复 "unimplemented"。
// 线程约定: 归属 platform 线程;处理器在 platform 线程运行。

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/macros.h"
#include "base/status.h"
#include "base/task_runner.h"
#include "embedder/common/host_channel.h"
#include "embedder/common/platform_menu.h"
#include "engine/common/message_channel_hub.h"

namespace momo::embedder {

class TrayChannel final : private TrayDelegate {
 public:
  static constexpr const char kName[] = "momo/tray";

  // menus 可为空;非空时必须比本对象活得久。
  TrayChannel(std::shared_ptr<engine::MessageChannelHub> hub, std::shared_ptr<TaskRunner> platform_runner,
              PlatformMenus* menus);
  ~TrayChannel();  // 移除全部图标

  // 处理一次 JS 调用(测试可直接调用)。
  StatusOr<engine::EncodableValue> HandleCall(const engine::MethodCall& call) { return channel_.Dispatch(call); }

 private:
  // 需要图标的方法: 解析 id, 未知 id 回复 notFound。
  using TrayHandler =
      std::function<StatusOr<engine::EncodableValue>(PlatformTray& tray, const engine::EncodableValue& args)>;
  void RegisterTrayMethod(std::string method, TrayHandler handler);

  // TrayDelegate
  void OnTrayClicked(int64_t tray_id, TrayClick click, const std::optional<Rect>& bounds) override;

  PlatformMenus* const menus_;
  HostChannel channel_;
  std::map<int64_t, std::unique_ptr<PlatformTray>> trays_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(TrayChannel);
};

}  // namespace momo::embedder
