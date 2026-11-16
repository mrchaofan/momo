#include "embedder/common/menu_channel.h"

#include <optional>
#include <utility>

namespace momo::embedder {

using engine::EncodableMap;
using engine::EncodableValue;
using namespace channel_args;

MenuChannel::MenuChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                         std::shared_ptr<TaskRunner> platform_runner, PlatformMenus* menus, Host* host)
    : menus_(menus), host_(host), channel_(std::move(hub), std::move(platform_runner), kName) {
  if (menus_ == nullptr) {
    return;  // 没有注册方法: 每个调用回复 unimplemented
  }
  menus_->SetDelegate(this);
  channel_.Register("setApplicationMenu", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    const EncodableValue* items = a.Find("items");
    if (items == nullptr || items->IsNull()) {
      menus_->SetApplicationMenu(nullptr);
      return EncodableValue();
    }
    MOMO_ASSIGN_OR_RETURN(MenuModel model, ParseMenuModel(*items));
    menus_->SetApplicationMenu(&model);
    return EncodableValue();
  });
  channel_.Register("popup", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    const EncodableValue* items = a.Find("items");
    MOMO_ASSIGN_OR_RETURN(MenuModel model, ParseMenuModel(items != nullptr ? *items : EncodableValue()));
    const std::optional<int64_t> view = Int(a, "view");
    PlatformWindow* window = view.has_value() ? host_->FindWindow(*view) : nullptr;
    const std::optional<double> x = OptNum(a, "x");
    const std::optional<double> y = OptNum(a, "y");
    std::optional<Point> position;
    if (window != nullptr && x.has_value() && y.has_value()) position = Point{*x, *y};
    const int64_t id = Int(a, "id").value_or(0);
    menus_->Popup(model, window, position, [this, id, alive = std::weak_ptr<bool>(alive_)] {
      if (!alive.expired()) channel_.Notify("popupClosed", EncodableValue(EncodableMap{{"id", id}}));
    });
    return EncodableValue(true);
  });
  channel_.Register("closePopup", [this](const EncodableValue&) -> StatusOr<EncodableValue> {
    menus_->ClosePopup();
    return EncodableValue();
  });
}

MenuChannel::~MenuChannel() {
  if (menus_ != nullptr) {
    menus_->ClosePopup();
    menus_->SetDelegate(nullptr);
  }
}

void MenuChannel::OnMenuItemActivated(int64_t item_id) {
  channel_.Notify("itemClicked", EncodableValue(EncodableMap{{"id", item_id}}));
}

void MenuChannel::OnMenuRoleActivated(MenuRole role) { host_->PerformMenuRole(role); }

}  // namespace momo::embedder
