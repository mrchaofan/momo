// MenuChannel / TrayChannel: momo/menu、momo/tray 协议的解析与分发(宿主侧, 不经引擎)。

#include "embedder/common/menu_channel.h"
#include "embedder/common/tray_channel.h"

#include <memory>
#include <string>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "embedder/common/testing/mock_platform.h"
#include "gtest/gtest.h"

namespace momo::embedder {
namespace {

using engine::EncodableList;
using engine::EncodableMap;
using engine::EncodableValue;
using engine::MessageChannelHub;
using engine::MethodCall;
using testing::MockMenus;
using testing::MockMenusState;
using testing::MockWindow;

class NullWindowDelegate final : public WindowDelegate {
 public:
  void OnMetricsChanged(const platform::ViewportMetrics&) override {}
  void OnPointerPacket(platform::PointerDataPacket) override {}
  void OnKeyData(const platform::KeyData&) override {}
  bool OnCloseRequested() override { return false; }
  void OnFocusChanged(bool) override {}
  void OnWindowStateChanged(const WindowStateFlags&) override {}
  void OnTextInput(const TextInputEvent&) override {}
};

class FakeHost final : public MenuChannel::Host {
 public:
  PlatformWindow* FindWindow(platform::ViewId view) override { return view == 7 ? &window : nullptr; }
  void PerformMenuRole(MenuRole role) override { roles.push_back(role); }

  NullWindowDelegate delegate;
  MockWindow window{WindowOptions{}, &delegate, nullptr};
  std::vector<MenuRole> roles;
};

EncodableValue Item(int64_t id, const char* type, const char* label, EncodableMap extra = {}) {
  // extra 在前: 同名键以它为准(按键查找取第一个)。
  for (auto& entry : EncodableMap{{"id", id}, {"type", type}, {"label", label}, {"enabled", true}, {"checked", false}}) {
    extra.push_back(std::move(entry));
  }
  return EncodableValue(std::move(extra));
}

class MenuChannelTest : public ::testing::Test {
 protected:
  MenuChannelTest() {
    // 测试充当 JS 端点: 记录宿主发往 JS 的通知(channel:method:id)。
    hub_->SetJsEndpoint(loop_.task_runner(),
                        [this](const std::string& channel, engine::MessageBytes message, engine::MessageReply) {
                          StatusOr<MethodCall> call = engine::DecodeMethodCall(message);
                          ASSERT_TRUE(call.ok());
                          const EncodableValue* id = call->args.Find("id");
                          to_js_.push_back(channel + ":" + call->method + ":" +
                                           std::to_string(id != nullptr ? id->AsInt().value_or(-1) : -1));
                        });
  }

  StatusOr<EncodableValue> Menu(std::string method, EncodableMap args = {}) {
    return menu_.HandleCall(MethodCall{std::move(method), EncodableValue(std::move(args))});
  }
  StatusOr<EncodableValue> Tray(std::string method, EncodableMap args = {}) {
    return tray_.HandleCall(MethodCall{std::move(method), EncodableValue(std::move(args))});
  }

  momo::testing::ScopedTestEventLoop loop_;
  std::shared_ptr<MessageChannelHub> hub_ = std::make_shared<MessageChannelHub>();
  std::shared_ptr<MockMenusState> state_ = std::make_shared<MockMenusState>();
  MockMenus menus_{state_};
  FakeHost host_;
  MenuChannel menu_{hub_, loop_.task_runner(), &menus_, &host_};
  TrayChannel tray_{hub_, loop_.task_runner(), &menus_};
  std::vector<std::string> to_js_;
};

TEST_F(MenuChannelTest, ApplicationMenuIsParsedAndCanBeReset) {
  EncodableList file{
      Item(2, "normal", "New", {{"accelerator", EncodableMap{{"key", "n"}, {"modifiers", 9}}}}),
      Item(3, "separator", ""),
      Item(4, "checkbox", "Wrap", {{"checked", true}, {"enabled", false}}),
      Item(5, "normal", "", {{"role", "close"}}),
  };
  ASSERT_TRUE(Menu("setApplicationMenu",
                   {{"items", EncodableList{Item(1, "submenu", "File", {{"submenu", std::move(file)}})}}})
                  .ok());
  ASSERT_TRUE(state_->application_menu.has_value());
  const MenuModel& model = *state_->application_menu;
  ASSERT_EQ(model.size(), 1u);
  EXPECT_EQ(model[0].type, MenuItemType::kSubmenu);
  EXPECT_EQ(model[0].label, "File");
  ASSERT_EQ(model[0].submenu.size(), 4u);
  const MenuItem& item = model[0].submenu[0];
  EXPECT_EQ(item.id, 2);
  EXPECT_EQ(item.accelerator, (MenuAccelerator{"n", 9}));
  EXPECT_EQ(model[0].submenu[1].type, MenuItemType::kSeparator);
  EXPECT_TRUE(model[0].submenu[2].checked);
  EXPECT_FALSE(model[0].submenu[2].enabled);
  EXPECT_EQ(model[0].submenu[3].role, MenuRole::kClose);

  ASSERT_TRUE(Menu("setApplicationMenu", {{"items", EncodableValue()}}).ok());
  EXPECT_FALSE(state_->application_menu.has_value());
  EXPECT_EQ(state_->application_menu_sets, 2);

  // 不认识的类型 / role: 整个调用失败, 不改动菜单。
  EXPECT_EQ(Menu("setApplicationMenu", {{"items", EncodableList{Item(1, "fancy", "x")}}}).status().code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(Menu("setApplicationMenu", {{"items", EncodableList{Item(1, "normal", "x", {{"role", "nope"}})}}})
                .status()
                .code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(state_->application_menu_sets, 2);
}

TEST_F(MenuChannelTest, PopupResolvesTheWindowAndPosition) {
  StatusOr<EncodableValue> opened = Menu(
      "popup", {{"id", 1}, {"items", EncodableList{Item(1, "normal", "Copy")}}, {"view", 7}, {"x", 12.0}, {"y", 34.0}});
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(*opened, EncodableValue(true));
  ASSERT_EQ(state_->popups.size(), 1u);
  EXPECT_EQ(state_->popup_window, &host_.window);
  ASSERT_TRUE(state_->popup_position.has_value());
  EXPECT_EQ(state_->popup_position->x, 12);
  EXPECT_EQ(state_->popup_position->y, 34);

  // 没有窗口(或未知视图): 在指针处弹出。打开下一个菜单时前一个关闭, JS 收到它的 popupClosed。
  ASSERT_TRUE(Menu("popup", {{"id", 2}, {"items", EncodableList{}}, {"view", 99}, {"x", 1.0}, {"y", 2.0}}).ok());
  EXPECT_EQ(state_->popup_window, nullptr);
  EXPECT_FALSE(state_->popup_position.has_value());
  loop_.RunUntilIdle();
  EXPECT_EQ(to_js_, (std::vector<std::string>{"momo/menu:popupClosed:1"}));

  ASSERT_TRUE(Menu("closePopup").ok());
  loop_.RunUntilIdle();
  EXPECT_EQ(to_js_.back(), "momo/menu:popupClosed:2");
  EXPECT_FALSE(state_->popup_closed);
}

TEST_F(MenuChannelTest, ActivationsReachJsAndRolesReachTheHost) {
  ASSERT_NE(state_->delegate, nullptr);
  state_->delegate->OnMenuItemActivated(42);
  state_->delegate->OnMenuRoleActivated(MenuRole::kQuit);
  loop_.RunUntilIdle();
  EXPECT_EQ(to_js_, (std::vector<std::string>{"momo/menu:itemClicked:42"}));
  EXPECT_EQ(host_.roles, (std::vector<MenuRole>{MenuRole::kQuit}));
}

TEST_F(MenuChannelTest, TrayLifecycleMethodsAndEvents) {
  ASSERT_TRUE(Tray("create", {{"id", 1}, {"image", "/icons/tray.png"}}).ok());
  ASSERT_TRUE(state_->trays.contains(1));
  EXPECT_EQ(state_->trays[1].image, "/icons/tray.png");
  EXPECT_EQ(Tray("create", {{"id", 1}, {"image", "/icons/tray.png"}}).status().code(), StatusCode::kInvalidArgument);
  // 平台拒绝图片: 图标不存在。
  EXPECT_FALSE(Tray("create", {{"id", 2}, {"image", ""}}).ok());
  EXPECT_FALSE(state_->trays.contains(2));
  EXPECT_EQ(Tray("setToolTip", {{"id", 2}, {"text", "x"}}).status().code(), StatusCode::kNotFound);

  ASSERT_TRUE(Tray("setToolTip", {{"id", 1}, {"text", "Momo"}}).ok());
  EXPECT_EQ(state_->trays[1].tooltip, "Momo");
  ASSERT_TRUE(Tray("setImage", {{"id", 1}, {"image", "/icons/other.png"}}).ok());
  EXPECT_EQ(state_->trays[1].image, "/icons/other.png");
  EXPECT_EQ(Tray("setTitle", {{"id", 1}, {"title", "3"}}).status().code(), StatusCode::kUnimplemented);
  ASSERT_TRUE(Tray("setContextMenu", {{"id", 1}, {"items", EncodableList{Item(5, "normal", "Show")}}}).ok());
  ASSERT_TRUE(state_->trays[1].menu.has_value());
  EXPECT_EQ((*state_->trays[1].menu)[0].label, "Show");
  ASSERT_TRUE(Tray("setContextMenu", {{"id", 1}, {"items", EncodableValue()}}).ok());
  EXPECT_FALSE(state_->trays[1].menu.has_value());
  StatusOr<EncodableValue> bounds = Tray("getBounds", {{"id", 1}});
  ASSERT_TRUE(bounds.ok());
  EXPECT_EQ(*bounds, EncodableValue(std::vector<double>{1, 2, 3, 4}));

  state_->trays[1].delegate->OnTrayClicked(1, TrayClick::kClick, Rect{1, 2, 3, 4});
  state_->trays[1].delegate->OnTrayClicked(1, TrayClick::kRightClick, std::nullopt);
  state_->trays[1].delegate->OnTrayClicked(1, TrayClick::kDoubleClick, std::nullopt);
  loop_.RunUntilIdle();
  EXPECT_EQ(to_js_, (std::vector<std::string>{"momo/tray:click:1", "momo/tray:rightClick:1", "momo/tray:doubleClick:1"}));

  ASSERT_TRUE(Tray("destroy", {{"id", 1}}).ok());
  EXPECT_TRUE(state_->trays.empty());
}

TEST(MenuChannelWithoutPlatformMenus, EveryMethodIsUnimplemented) {
  momo::testing::ScopedTestEventLoop loop;
  auto hub = std::make_shared<MessageChannelHub>();
  FakeHost host;
  MenuChannel menu(hub, loop.task_runner(), nullptr, &host);
  TrayChannel tray(hub, loop.task_runner(), nullptr);
  EXPECT_EQ(menu.HandleCall({"setApplicationMenu", EncodableValue()}).status().code(), StatusCode::kUnimplemented);
  EXPECT_EQ(tray.HandleCall({"create", EncodableValue()}).status().code(), StatusCode::kUnimplemented);
}

}  // namespace
}  // namespace momo::embedder
