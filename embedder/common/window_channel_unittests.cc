// WindowChannel: momo/window 协议的解析与分发(宿主侧, 不经引擎)。

#include "embedder/common/window_channel.h"

#include <map>
#include <memory>
#include <optional>
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
using engine::MessageBytes;
using engine::MessageChannelHub;
using engine::MessageReply;
using engine::MethodCall;
using testing::MockWindow;

class NullDelegate final : public WindowDelegate {
 public:
  void OnMetricsChanged(const platform::ViewportMetrics&) override {}
  void OnPointerPacket(platform::PointerDataPacket) override {}
  void OnKeyData(const platform::KeyData&) override {}
  bool OnCloseRequested() override { return false; }
  void OnFocusChanged(bool) override {}
  void OnVisibilityChanged(bool) override {}
  void OnTextInput(const TextInputEvent&) override {}
  void OnWindowControlsChanged(const std::optional<Rect>& rect) override { controls = rect; }
  std::optional<Rect> controls;
};

// 只实现必需方法的窗口: 全部可选能力都不支持。
class BareWindow final : public PlatformWindow {
 public:
  void Show() override {}
  void SetTitle(std::string_view) override {}
  void SetMinSize(Size) override {}
  Size physical_size() const override { return {}; }
  double device_pixel_ratio() const override { return 1; }
  platform::RenderSurface& surface() override { return surface_; }
  void SetCursor(SystemCursor) override {}
  TextInputConnection& text_input() override { return input_; }
  void* native_handle() const override { return nullptr; }

 private:
  testing::MockSurface surface_;
  testing::MockTextInput input_;
};

class FakeHost final : public WindowChannel::Host {
 public:
  StatusOr<platform::ViewId> CreateWindow(const WindowOptions& options) override {
    created.push_back(options);
    const platform::ViewId id = next_id++;
    WindowSpec spec;
    spec.size = options.size;
    spec.title = options.title;
    windows[id] = std::make_unique<MockWindow>(spec, &delegate, nullptr);
    return id;
  }
  void CloseWindow(platform::ViewId view) override {
    closed.push_back(view);
    windows.erase(view);
  }
  PlatformWindow* FindWindow(platform::ViewId view) override {
    if (view == kBareView) return &bare;
    auto it = windows.find(view);
    return it != windows.end() ? it->second.get() : nullptr;
  }
  MockWindow* mock(platform::ViewId view) { return windows.at(view).get(); }

  static constexpr platform::ViewId kBareView = 99;
  NullDelegate delegate;
  BareWindow bare;
  std::map<platform::ViewId, std::unique_ptr<MockWindow>> windows;
  std::vector<WindowOptions> created;
  std::vector<platform::ViewId> closed;
  platform::ViewId next_id = 1;
};

EncodableValue Args(EncodableMap map) { return EncodableValue(std::move(map)); }

class WindowChannelTest : public ::testing::Test {
 protected:
  WindowChannelTest()
      : hub_(std::make_shared<MessageChannelHub>()),
        channel_(hub_, loop_.task_runner(), &host_) {}

  StatusOr<EncodableValue> Call(std::string method, EncodableMap args = {}) {
    return channel_.HandleCall(MethodCall{std::move(method), Args(std::move(args))});
  }
  platform::ViewId Create() {
    StatusOr<EncodableValue> id = Call("create", {{"width", 300.0}, {"height", 200.0}});
    EXPECT_TRUE(id.ok()) << id.status();
    return id.ok() ? *id->AsInt() : -1;
  }

  momo::testing::ScopedTestEventLoop loop_;
  std::shared_ptr<MessageChannelHub> hub_;
  FakeHost host_;
  WindowChannel channel_;
};

TEST_F(WindowChannelTest, CreateParsesOptions) {
  StatusOr<EncodableValue> id = Call("create", {{"title", "doc"},
                                                {"width", 640.0},
                                                {"height", 480},
                                                {"minWidth", 320.0},
                                                {"x", 10.0},
                                                {"y", 20.0},
                                                {"nonActivating", true},
                                                {"resizable", false}});
  ASSERT_TRUE(id.ok()) << id.status();
  EXPECT_EQ(id->AsInt(), 1);
  ASSERT_EQ(host_.created.size(), 1u);
  const WindowOptions& o = host_.created[0];
  EXPECT_EQ(o.title, "doc");
  EXPECT_EQ(o.size, (Size{640, 480}));
  EXPECT_EQ(o.min_size.width, 320);
  ASSERT_TRUE(o.position.has_value());
  EXPECT_EQ(o.position->x, 10);
  EXPECT_TRUE(o.non_activating);
  EXPECT_FALSE(o.resizable);

  EXPECT_EQ(Call("create", {{"width", 0.0}, {"height", 10.0}}).status().code(),
            StatusCode::kInvalidArgument);
}

TEST_F(WindowChannelTest, RequestsReachTheWindow) {
  const platform::ViewId view = Create();
  MockWindow* w = host_.mock(view);
  ASSERT_TRUE(Call("setTitle", {{"view", view}, {"title", "hello"}}).ok());
  EXPECT_EQ(w->spec().title, "hello");
  ASSERT_TRUE(Call("setCursor", {{"view", view}, {"cursor", "text"}}).ok());
  EXPECT_EQ(w->cursor(), SystemCursor::kIBeam);
  EXPECT_EQ(Call("setCursor", {{"view", view}, {"cursor", "nope"}}).status().code(),
            StatusCode::kInvalidArgument);
  ASSERT_TRUE(Call("setAlwaysOnTop", {{"view", view}, {"on", true}}).ok());
  EXPECT_TRUE(w->always_on_top());
  ASSERT_TRUE(Call("activate", {{"view", view}}).ok());
  EXPECT_EQ(w->activations(), 1);
  ASSERT_TRUE(Call("setPosition", {{"view", view}, {"x", 40.0}, {"y", 50.0}}).ok());
  EXPECT_EQ(w->screen_frame().x, 40);
  ASSERT_TRUE(Call("startDrag", {{"view", view}, {"sequenceId", 7}}).ok());
  EXPECT_EQ(w->drags(), (std::vector<int64_t>{7}));
  ASSERT_TRUE(Call("startMoveLoop", {{"view", view}, {"x", 12.0}, {"y", 8.0}}).ok());
  ASSERT_TRUE(w->move_loop().has_value());
  EXPECT_EQ(w->move_loop()->x, 12);
  ASSERT_TRUE(Call("endMoveLoop", {{"view", view}}).ok());
  EXPECT_FALSE(w->move_loop().has_value());
  StatusOr<EncodableValue> captured = Call("capturePointer", {{"view", view}, {"sequenceId", 3}});
  ASSERT_TRUE(captured.ok());
  EXPECT_EQ(*captured, EncodableValue(true));
  EXPECT_EQ(w->captures(), 1);
}

// setResizable 与 setResizeBorder 各改缩放行为的一部分, 与当前值合并(互不覆盖);负数边框按 0。
TEST_F(WindowChannelTest, ResizableAndResizeBorderMerge) {
  const platform::ViewId view = Create();
  MockWindow* w = host_.mock(view);
  ASSERT_TRUE(Call("setResizable", {{"view", view}, {"resizable", false}}).ok());
  EXPECT_FALSE(w->resize_options().resizable);
  ASSERT_TRUE(Call("setResizeBorder", {{"view", view}, {"top", 0.0}, {"left", 8.0}, {"bottom", -2.0}}).ok());
  EXPECT_FALSE(w->resize_options().resizable) << "the border does not reset resizable";
  EXPECT_EQ(w->resize_options().border_top, 0);
  EXPECT_EQ(w->resize_options().border_left, 8);
  EXPECT_EQ(w->resize_options().border_bottom, 0);
  EXPECT_EQ(w->resize_options().border_right, WindowResizeOptions{}.border_right);
  ASSERT_TRUE(Call("setResizable", {{"view", view}, {"resizable", true}}).ok());
  EXPECT_EQ(w->resize_options().border_left, 8) << "resizable does not reset the border";
}

// 隐藏标题栏: 应用只给出自己的标题栏高度;系统控制按钮的占用范围由宿主报告(getState / controlsChanged)。
TEST_F(WindowChannelTest, TitleBarReportsWindowControls) {
  const platform::ViewId view = Create();
  StatusOr<EncodableValue> state = Call("getState", {{"view", view}});
  ASSERT_TRUE(state.ok());
  EXPECT_TRUE(state->Find("controls")->IsNull());
  ASSERT_TRUE(Call("setTitleBar", {{"view", view}, {"hidden", true}, {"height", 40.0}}).ok());
  EXPECT_TRUE(host_.mock(view)->title_bar().hidden);
  EXPECT_EQ(host_.mock(view)->title_bar().height, 40);
  ASSERT_TRUE(host_.delegate.controls.has_value());
  state = Call("getState", {{"view", view}});
  ASSERT_TRUE(state.ok());
  const auto* controls = state->Find("controls")->GetIf<std::vector<double>>();
  ASSERT_NE(controls, nullptr);
  EXPECT_EQ(*controls, (std::vector<double>{0, 0, 70, 40}));
  const auto* frame = state->Find("frame")->GetIf<std::vector<double>>();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ((*frame)[2], 300);
  const auto* caps = state->Find("capabilities")->GetIf<EncodableList>();
  ASSERT_NE(caps, nullptr);
  EXPECT_NE(std::find(caps->begin(), caps->end(), EncodableValue("hiddenTitleBar")), caps->end());
}

// 不支持的能力明确报错(而不是静默无效), 能力可查询。
TEST_F(WindowChannelTest, UnsupportedCapabilitiesAreReported) {
  const platform::ViewId bare = FakeHost::kBareView;
  EXPECT_EQ(Call("setAlwaysOnTop", {{"view", bare}, {"on", true}}).status().code(),
            StatusCode::kUnimplemented);
  EXPECT_EQ(Call("setTitleBar", {{"view", bare}, {"hidden", true}}).status().code(),
            StatusCode::kUnimplemented);
  EXPECT_EQ(Call("startDrag", {{"view", bare}, {"sequenceId", 1}}).status().code(),
            StatusCode::kUnimplemented);
  StatusOr<EncodableValue> state = Call("getState", {{"view", bare}});
  ASSERT_TRUE(state.ok());
  EXPECT_TRUE(state->Find("capabilities")->GetIf<EncodableList>()->empty());
  EXPECT_TRUE(state->Find("frame")->IsNull());
  // 必需能力照常可用。
  EXPECT_TRUE(Call("setTitle", {{"view", bare}, {"title", "x"}}).ok());
}

TEST_F(WindowChannelTest, UnknownViewsAndMethods) {
  EXPECT_EQ(Call("setTitle", {{"view", 42}, {"title", "x"}}).status().code(), StatusCode::kNotFound);
  EXPECT_EQ(Call("setTitle", {{"title", "x"}}).status().code(), StatusCode::kInvalidArgument);
  const platform::ViewId view = Create();
  EXPECT_EQ(Call("teleport", {{"view", view}}).status().code(), StatusCode::kUnimplemented);
  // close 未知视图: 空操作(可能已被用户关闭)。
  EXPECT_TRUE(Call("close", {{"view", 42}}).ok());
  EXPECT_TRUE(Call("close", {{"view", view}}).ok());
  EXPECT_EQ(host_.closed, (std::vector<platform::ViewId>{42, view}));
}

// 关闭/退出询问: JS 无处理器时允许;JS 回复 false 时取消。
TEST_F(WindowChannelTest, CloseAndExitRequestsAskJs) {
  std::optional<bool> allowed;
  channel_.RequestClose(1, [&](bool allow) { allowed = allow; });
  loop_.RunUntilIdle();
  EXPECT_EQ(allowed, true) << "no JS endpoint: allow";

  std::vector<std::string> asked;
  hub_->SetJsEndpoint(loop_.task_runner(),
                      [&](const std::string& channel, MessageBytes message, MessageReply reply) {
                        StatusOr<MethodCall> call = engine::DecodeMethodCall(message);
                        ASSERT_TRUE(call.ok());
                        asked.push_back(channel + ":" + call->method);
                        reply.Send(engine::EncodeSuccessEnvelope(EncodableValue(false)));
                      });
  allowed.reset();
  channel_.RequestClose(1, [&](bool allow) { allowed = allow; });
  loop_.RunUntilIdle();
  EXPECT_EQ(allowed, false);
  allowed.reset();
  channel_.RequestAppExit([&](bool allow) { allowed = allow; });
  loop_.RunUntilIdle();
  EXPECT_EQ(allowed, false);
  EXPECT_EQ(asked, (std::vector<std::string>{"momo/window:closeRequested", "momo/app:requestExit"}));
  hub_->SetJsEndpoint(nullptr, nullptr);
}

}  // namespace
}  // namespace momo::embedder
