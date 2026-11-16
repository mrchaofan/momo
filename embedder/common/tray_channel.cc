#include "embedder/common/tray_channel.h"

#include <string>
#include <utility>

namespace momo::embedder {

using engine::EncodableMap;
using engine::EncodableValue;
using namespace channel_args;

TrayChannel::TrayChannel(std::shared_ptr<engine::MessageChannelHub> hub,
                         std::shared_ptr<TaskRunner> platform_runner, PlatformMenus* menus)
    : menus_(menus), channel_(std::move(hub), std::move(platform_runner), kName) {
  if (menus_ == nullptr) {
    return;  // 没有注册方法: 每个调用回复 unimplemented
  }
  channel_.Register("create", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    const std::optional<int64_t> id = Int(a, "id");
    if (!id.has_value() || trays_.contains(*id)) return InvalidArgumentError("create: missing or duplicate tray id");
    MOMO_ASSIGN_OR_RETURN(trays_[*id], menus_->CreateTray(*id, Str(a, "image"), this));
    return EncodableValue();
  });
  channel_.Register("destroy", [this](const EncodableValue& a) -> StatusOr<EncodableValue> {
    trays_.erase(Int(a, "id").value_or(-1));
    return EncodableValue();
  });
  RegisterTrayMethod("setImage", [](PlatformTray& tray, const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_RETURN_IF_ERROR(tray.SetImage(Str(a, "image")));
    return EncodableValue();
  });
  RegisterTrayMethod("setToolTip", [](PlatformTray& tray, const EncodableValue& a) -> StatusOr<EncodableValue> {
    tray.SetToolTip(Str(a, "text"));
    return EncodableValue();
  });
  RegisterTrayMethod("setTitle", [](PlatformTray& tray, const EncodableValue& a) -> StatusOr<EncodableValue> {
    MOMO_RETURN_IF_ERROR(tray.SetTitle(Str(a, "title")));
    return EncodableValue();
  });
  RegisterTrayMethod("setContextMenu", [](PlatformTray& tray, const EncodableValue& a) -> StatusOr<EncodableValue> {
    const EncodableValue* items = a.Find("items");
    if (items == nullptr || items->IsNull()) {
      tray.SetContextMenu(nullptr);
      return EncodableValue();
    }
    MOMO_ASSIGN_OR_RETURN(MenuModel model, ParseMenuModel(*items));
    tray.SetContextMenu(&model);
    return EncodableValue();
  });
  RegisterTrayMethod("getBounds", [](PlatformTray& tray, const EncodableValue&) -> StatusOr<EncodableValue> {
    return OptRectValue(tray.bounds());
  });
}

TrayChannel::~TrayChannel() = default;

void TrayChannel::RegisterTrayMethod(std::string method, TrayHandler handler) {
  channel_.Register(std::move(method),
                    [this, handler = std::move(handler)](const EncodableValue& a) -> StatusOr<EncodableValue> {
                      auto it = trays_.find(Int(a, "id").value_or(-1));
                      if (it == trays_.end()) return NotFoundError("no such tray");
                      return handler(*it->second, a);
                    });
}

void TrayChannel::OnTrayClicked(int64_t tray_id, TrayClick click, const std::optional<Rect>& bounds) {
  const char* method = click == TrayClick::kClick        ? "click"
                       : click == TrayClick::kRightClick ? "rightClick"
                                                         : "doubleClick";
  channel_.Notify(method, EncodableValue(EncodableMap{{"id", tray_id}, {"bounds", OptRectValue(bounds)}}));
}

}  // namespace momo::embedder
