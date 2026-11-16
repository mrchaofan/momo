#include "embedder/common/menu_model.h"

#include <string_view>
#include <utility>

namespace momo::embedder {

using engine::EncodableList;
using engine::EncodableValue;

namespace {

constexpr std::pair<std::string_view, MenuItemType> kTypes[] = {
    {"normal", MenuItemType::kNormal},     {"separator", MenuItemType::kSeparator},
    {"submenu", MenuItemType::kSubmenu},   {"checkbox", MenuItemType::kCheckbox},
    {"radio", MenuItemType::kRadio},
};

constexpr std::pair<std::string_view, MenuRole> kRoles[] = {
    {"about", MenuRole::kAbout},           {"services", MenuRole::kServices},
    {"hide", MenuRole::kHide},             {"hideOthers", MenuRole::kHideOthers},
    {"unhide", MenuRole::kUnhide},         {"quit", MenuRole::kQuit},
    {"undo", MenuRole::kUndo},             {"redo", MenuRole::kRedo},
    {"cut", MenuRole::kCut},               {"copy", MenuRole::kCopy},
    {"paste", MenuRole::kPaste},           {"selectAll", MenuRole::kSelectAll},
    {"minimize", MenuRole::kMinimize},     {"zoom", MenuRole::kZoom},
    {"close", MenuRole::kClose},           {"front", MenuRole::kFront},
    {"appMenu", MenuRole::kAppMenu},       {"editMenu", MenuRole::kEditMenu},
    {"windowMenu", MenuRole::kWindowMenu},
};

template <typename T, size_t N>
StatusOr<T> Lookup(const std::pair<std::string_view, T> (&table)[N], std::string_view name, const char* what) {
  for (const auto& [n, value] : table) {
    if (n == name) return value;
  }
  return InvalidArgumentError(std::string("unknown menu item ") + what + " '" + std::string(name) + "'");
}

StatusOr<MenuItem> ParseItem(const EncodableValue& v) {
  MenuItem item;
  auto str = [&v](std::string_view key) {
    const EncodableValue* f = v.Find(key);
    return f != nullptr ? f->AsString().value_or("") : std::string();
  };
  auto flag = [&v](std::string_view key, bool fallback) {
    const EncodableValue* f = v.Find(key);
    const bool* b = f != nullptr ? f->GetIf<bool>() : nullptr;
    return b != nullptr ? *b : fallback;
  };
  if (const EncodableValue* id = v.Find("id")) item.id = id->AsInt().value_or(0);
  MOMO_ASSIGN_OR_RETURN(item.type, Lookup(kTypes, str("type"), "type"));
  if (const std::string role = str("role"); !role.empty()) {
    MOMO_ASSIGN_OR_RETURN(item.role, Lookup(kRoles, role, "role"));
  }
  item.label = str("label");
  item.enabled = flag("enabled", true);
  item.checked = flag("checked", false);
  if (const EncodableValue* a = v.Find("accelerator"); a != nullptr && !a->IsNull()) {
    MenuAccelerator accelerator;
    if (const EncodableValue* key = a->Find("key")) accelerator.key = key->AsString().value_or("");
    if (const EncodableValue* m = a->Find("modifiers")) {
      accelerator.modifiers = static_cast<uint32_t>(m->AsInt().value_or(0));
    }
    if (accelerator.key.empty()) return InvalidArgumentError("menu item accelerator without a key");
    item.accelerator = std::move(accelerator);
  }
  if (const EncodableValue* submenu = v.Find("submenu"); submenu != nullptr && !submenu->IsNull()) {
    MOMO_ASSIGN_OR_RETURN(item.submenu, ParseMenuModel(*submenu));
  }
  return item;
}

}  // namespace

StatusOr<MenuModel> ParseMenuModel(const EncodableValue& items) {
  const auto* list = items.GetIf<EncodableList>();
  if (list == nullptr) return InvalidArgumentError("menu items must be a list");
  MenuModel model;
  model.reserve(list->size());
  for (const EncodableValue& v : *list) {
    MOMO_ASSIGN_OR_RETURN(MenuItem item, ParseItem(v));
    model.push_back(std::move(item));
  }
  return model;
}

}  // namespace momo::embedder
