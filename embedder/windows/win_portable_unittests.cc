// Windows 平台实现中不依赖 <windows.h> 的部分: 在所有平台编译并运行(macOS 上即可验证)。
//   扫描码 → HID 映射、鼠标消息翻译(+ PointerDispatcher 端到端)、多 DPI 屏幕坐标布局、窗口框架命中与标题栏按钮布局。
// 依赖 Win32 的部分(窗口、消息循环、vsync、剪贴板)在 win_platform_unittests.cc, 只在 Windows 上编译。

#include <vector>

#include "embedder/windows/win_frame.h"
#include "embedder/windows/win_key_mapping.h"
#include "embedder/windows/win_mouse_events.h"
#include "embedder/windows/win_screen_layout.h"
#include "gtest/gtest.h"

namespace momo::embedder::windows {
namespace {

using platform::PointerDeviceKind;
using platform::PointerPhase;

// ---- 扫描码映射 ----

TEST(WinKeyMappingTest, MapsToUsbHidUsages) {
  EXPECT_EQ(WinScanCodeToHidUsage(0x1E, false), 0x04u);  // A
  EXPECT_EQ(WinScanCodeToHidUsage(0x2C, false), 0x1Du);  // Z
  EXPECT_EQ(WinScanCodeToHidUsage(0x0B, false), 0x27u);  // 0
  EXPECT_EQ(WinScanCodeToHidUsage(0x1C, false), 0x28u);  // Enter
  EXPECT_EQ(WinScanCodeToHidUsage(0x1C, true), 0x58u);   // NumpadEnter
  EXPECT_EQ(WinScanCodeToHidUsage(0x1D, false), 0xE0u);  // ControlLeft
  EXPECT_EQ(WinScanCodeToHidUsage(0x1D, true), 0xE4u);   // ControlRight
  EXPECT_EQ(WinScanCodeToHidUsage(0x38, true), 0xE6u);   // AltRight
  EXPECT_EQ(WinScanCodeToHidUsage(0x5B, true), 0xE3u);   // MetaLeft
  EXPECT_EQ(WinScanCodeToHidUsage(0x48, true), 0x52u);   // ArrowUp
  EXPECT_EQ(WinScanCodeToHidUsage(0x48, false), 0x60u);  // Numpad8
  EXPECT_EQ(WinScanCodeToHidUsage(0x45, true), 0x53u);   // NumLock
  EXPECT_EQ(WinScanCodeToHidUsage(0x45, false), 0x48u);  // Pause
  EXPECT_EQ(WinScanCodeToHidUsage(0x3B, false), 0x3Au);  // F1
  EXPECT_EQ(WinScanCodeToHidUsage(0x76, false), 0x73u);  // F24
  EXPECT_EQ(WinScanCodeToHidUsage(0x00, false), 0u);
  EXPECT_EQ(WinScanCodeToHidUsage(0x7F, true), 0u);
  EXPECT_EQ(PhysicalKeyFromHidUsage(0x04), 0x00070004u);
  EXPECT_EQ(PhysicalKeyFromHidUsage(0), 0u);
}

TEST(WinKeyMappingTest, ClassifiesSpecialKeys) {
  EXPECT_TRUE(IsSpecialHidUsage(0x28));   // Enter
  EXPECT_TRUE(IsSpecialHidUsage(0x29));   // Escape
  EXPECT_TRUE(IsSpecialHidUsage(0xE1));   // ShiftLeft
  EXPECT_TRUE(IsSpecialHidUsage(0x3A));   // F1
  EXPECT_TRUE(IsSpecialHidUsage(0x52));   // ArrowUp
  EXPECT_TRUE(IsSpecialHidUsage(0x58));   // NumpadEnter
  EXPECT_FALSE(IsSpecialHidUsage(0x04));  // A
  EXPECT_FALSE(IsSpecialHidUsage(0x2C));  // Space
  EXPECT_FALSE(IsSpecialHidUsage(0x62));  // Numpad0
  EXPECT_FALSE(IsSpecialHidUsage(0x67));  // NumpadEqual
}

// ---- 鼠标消息翻译 ----

WinMouseMessage Msg(uint32_t message, uint64_t wparam, Point screen = {10, 20}) {
  WinMouseMessage m;
  m.message = message;
  m.wparam = wparam;
  m.screen = screen;
  m.timestamp_us = 1000;
  return m;
}

uint64_t WheelWParam(int delta, uint32_t keys = 0) {
  return (static_cast<uint64_t>(static_cast<uint16_t>(static_cast<int16_t>(delta))) << 16) | keys;
}

TEST(WinMouseEventsTest, TranslatesButtons) {
  PointerInput in;
  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kLButtonDown, win32::kMkLButton), nullptr, in));
  EXPECT_EQ(in.type, PointerInput::Type::kDown);
  EXPECT_EQ(in.button, 1u);
  EXPECT_FALSE(in.others_held);  // 按下消息的状态里包含本键本身
  EXPECT_EQ(in.data.screen_x, 10);
  EXPECT_EQ(in.data.screen_y, 20);

  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kRButtonDown, win32::kMkLButton | win32::kMkRButton), nullptr, in));
  EXPECT_EQ(in.button, 2u);
  EXPECT_TRUE(in.others_held);

  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kLButtonUp, 0), nullptr, in));
  EXPECT_EQ(in.type, PointerInput::Type::kUp);
  EXPECT_FALSE(in.others_held);

  // 侧键: HIWORD = XBUTTON1 / XBUTTON2。
  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kXButtonDown, (1u << 16) | win32::kMkXButton1), nullptr, in));
  EXPECT_EQ(in.button, 1u << 3);
  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kXButtonUp, 2u << 16), nullptr, in));
  EXPECT_EQ(in.button, 1u << 4);

  // 双击消息是第二次按下。
  const WinMouseMessage dbl = Msg(win32::kLButtonDblClk, win32::kMkLButton);
  ASSERT_TRUE(WinMouseInput::Translate(dbl, nullptr, in));
  EXPECT_EQ(in.type, PointerInput::Type::kDown);
  EXPECT_EQ(dbl.click_count(), 2);
  EXPECT_EQ(Msg(win32::kLButtonDown, 0).click_count(), 1);
}

TEST(WinMouseEventsTest, TranslatesMoveLeaveAndDeviceKind) {
  PointerInput in;
  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kMouseMove, win32::kMkMButton), nullptr, in));
  EXPECT_EQ(in.type, PointerInput::Type::kMove);
  EXPECT_TRUE(in.others_held);
  EXPECT_EQ(in.data.kind, PointerDeviceKind::kMouse);

  WinMouseMessage pen = Msg(win32::kMouseMove, 0);
  pen.extra_info = win32::kPenSignature | 0x01;
  ASSERT_TRUE(WinMouseInput::Translate(pen, nullptr, in));
  EXPECT_EQ(in.data.kind, PointerDeviceKind::kStylus);
  pen.extra_info = win32::kPenSignature | win32::kTouchFlag;
  ASSERT_TRUE(WinMouseInput::Translate(pen, nullptr, in));
  EXPECT_EQ(in.data.kind, PointerDeviceKind::kTouch);

  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kMouseLeave, 0), nullptr, in));
  EXPECT_EQ(in.type, PointerInput::Type::kLeave);

  EXPECT_FALSE(WinMouseInput::Translate(Msg(0x0100 /* WM_KEYDOWN */, 0), nullptr, in));
}

TEST(WinMouseEventsTest, TranslatesWheelToViewportDirection) {
  PointerInput in;
  // 向前滚一格(远离用户)= 视口向上滚: 负 y;默认 3 行 × 40 点。
  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kMouseWheel, WheelWParam(120)), nullptr, in));
  EXPECT_EQ(in.type, PointerInput::Type::kScroll);
  EXPECT_DOUBLE_EQ(in.data.scroll_delta_y, -120);
  EXPECT_DOUBLE_EQ(in.data.scroll_delta_x, 0);
  EXPECT_EQ(in.data.kind, PointerDeviceKind::kMouse);

  // Shift + 垂直滚轮 = 水平。
  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kMouseWheel, WheelWParam(-120, win32::kMkShift)), nullptr, in));
  EXPECT_DOUBLE_EQ(in.data.scroll_delta_x, 120);
  EXPECT_DOUBLE_EQ(in.data.scroll_delta_y, 0);

  // 水平滚轮: 正 = 向右。
  WinMouseMessage h = Msg(win32::kMouseHWheel, WheelWParam(120));
  h.wheel_chars = 2;
  ASSERT_TRUE(WinMouseInput::Translate(h, nullptr, in));
  EXPECT_DOUBLE_EQ(in.data.scroll_delta_x, 80);

  // 精确触控板: 非整格增量。
  ASSERT_TRUE(WinMouseInput::Translate(Msg(win32::kMouseWheel, WheelWParam(-30)), nullptr, in));
  EXPECT_EQ(in.data.kind, PointerDeviceKind::kTrackpad);
  EXPECT_DOUBLE_EQ(in.data.scroll_delta_y, 30);

  // 整页滚动。
  WinMouseMessage page = Msg(win32::kMouseWheel, WheelWParam(-120));
  page.wheel_lines = win32::kWheelPageScroll;
  page.page_height = 500;
  ASSERT_TRUE(WinMouseInput::Translate(page, nullptr, in));
  EXPECT_DOUBLE_EQ(in.data.scroll_delta_y, 500);

  // 零增量 / 系统设置为 0 行: 没有输入。
  EXPECT_FALSE(WinMouseInput::Translate(Msg(win32::kMouseWheel, WheelWParam(0)), nullptr, in));
  WinMouseMessage none = Msg(win32::kMouseWheel, WheelWParam(120));
  none.wheel_lines = 0;
  EXPECT_FALSE(WinMouseInput::Translate(none, nullptr, in));
}

// 派发器眼里的一个窗口: 内容区为屏幕上的 frame, 记录交给 momo 的事件。
class FakeWindow final : public PointerWindow {
 public:
  explicit FakeWindow(Rect frame) : frame_(frame) {}
  std::optional<int64_t> HitTest(Point s) const override {
    if (s.x < frame_.x || s.y < frame_.y || s.x >= frame_.right() || s.y >= frame_.bottom()) return std::nullopt;
    return 0;
  }
  void SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) override {
    for (const platform::PointerData& d : packet) {
      phases.push_back(d.phase);
      sequences.push_back(d.sequence_id);
      buttons.push_back(d.buttons);
      if (d.phase == PointerPhase::kDown && input.native != nullptr) {
        click_counts.push_back(static_cast<const WinMouseMessage*>(input.native)->click_count());
      }
    }
  }
  void SendToPlatformView(int64_t, const PointerInput&, PointerPhase) override {}
  void SetHoveredPlatformView(int64_t, const PointerInput&) override {}
  void MoveTo(Point top_left) override {
    frame_.x = top_left.x;
    frame_.y = top_left.y;
    moves++;
  }

  Rect frame_;
  std::vector<PointerPhase> phases;
  std::vector<int64_t> sequences;
  std::vector<uint32_t> buttons;
  std::vector<int> click_counts;
  int moves = 0;
};

TEST(WinMouseInputTest, PressSequenceThroughDispatcher) {
  WinMouseInput mouse;
  FakeWindow window({0, 0, 100, 100});
  mouse.Dispatch(Msg(win32::kMouseMove, 0, {10, 10}), &window);
  mouse.Dispatch(Msg(win32::kLButtonDown, win32::kMkLButton, {10, 10}), &window);
  EXPECT_TRUE(mouse.dispatcher().has_sequence());
  // 捕获期间指针拖到窗口外: 仍交给按下所在的窗口。
  mouse.Dispatch(Msg(win32::kMouseMove, win32::kMkLButton, {150, 10}), &window);
  mouse.Dispatch(Msg(win32::kLButtonUp, 0, {150, 10}), &window);
  EXPECT_FALSE(mouse.dispatcher().has_sequence());
  const std::vector<PointerPhase> expected = {PointerPhase::kAdd, PointerPhase::kHover, PointerPhase::kDown,
                                              PointerPhase::kMove, PointerPhase::kUp, PointerPhase::kRemove};
  EXPECT_EQ(window.phases, expected);
  // 同一序列的事件带同一个 id, 序列外为 0。
  EXPECT_EQ(window.sequences[1], 0);
  EXPECT_GT(window.sequences[2], 0);
  EXPECT_EQ(window.sequences[3], window.sequences[2]);
  EXPECT_EQ(window.buttons[3], 1u);
  ASSERT_EQ(window.click_counts.size(), 1u);
  EXPECT_EQ(window.click_counts[0], 1);
}

TEST(WinMouseInputTest, StaleSequenceEndsWhenNoButtonIsHeld) {
  // 抬起被系统的模态循环吞掉(系统拖动、菜单): 下一条消息的按键状态为空, 派发器补报抬起。
  WinMouseInput mouse;
  FakeWindow window({0, 0, 100, 100});
  mouse.Dispatch(Msg(win32::kLButtonDown, win32::kMkLButton, {10, 10}), &window);
  mouse.Dispatch(Msg(win32::kMouseMove, 0, {20, 10}), &window);
  EXPECT_FALSE(mouse.dispatcher().has_sequence());
  ASSERT_GE(window.phases.size(), 3u);
  EXPECT_EQ(window.phases[1], PointerPhase::kDown);
  EXPECT_EQ(window.phases[2], PointerPhase::kUp);
}

TEST(WinMouseInputTest, MoveLoopMovesWindowWithPointer) {
  WinMouseInput mouse;
  FakeWindow window({0, 0, 100, 100});
  mouse.Dispatch(Msg(win32::kLButtonDown, win32::kMkLButton, {10, 10}), &window);
  // 开始时立即把窗口对齐到指针, 之后每个拖动都移动窗口(宿主同步完成, 不经 ui 线程)。
  ASSERT_TRUE(mouse.dispatcher().StartMoveLoop(&window, {10, 10}));
  EXPECT_EQ(window.moves, 1);
  mouse.Dispatch(Msg(win32::kMouseMove, win32::kMkLButton, {60, 40}), &window);
  EXPECT_EQ(window.moves, 2);
  EXPECT_EQ(window.frame_.x, 50);
  EXPECT_EQ(window.frame_.y, 30);
}

TEST(WinMouseInputTest, CaptureMovesSequenceToAnotherWindow) {
  WinMouseInput mouse;
  FakeWindow a({0, 0, 100, 100});
  FakeWindow b({200, 0, 100, 100});
  mouse.Dispatch(Msg(win32::kLButtonDown, win32::kMkLButton, {10, 10}), &a);
  const int64_t id = a.sequences.back();
  EXPECT_FALSE(mouse.dispatcher().Capture(&b, id + 1));
  EXPECT_TRUE(mouse.dispatcher().Capture(&b, id));
  // 系统仍把消息交给原捕获窗口 a: 派发器转给 b。
  mouse.Dispatch(Msg(win32::kMouseMove, win32::kMkLButton, {250, 10}), &a);
  mouse.Dispatch(Msg(win32::kLButtonUp, 0, {250, 10}), &a);
  // 改交不合成事件(框架的逻辑接收方不变): b 直接收到同一序列的拖动与抬起。
  ASSERT_EQ(b.phases.size(), 2u);
  EXPECT_EQ(b.phases[0], PointerPhase::kMove);
  EXPECT_EQ(b.sequences[0], id);
  EXPECT_EQ(b.phases[1], PointerPhase::kUp);
}

// ---- 屏幕坐标布局 ----

WinMonitorInfo Monitor(uint64_t id, Rect physical, double dpr, bool primary = false) {
  WinMonitorInfo m;
  m.id = id;
  m.physical = physical;
  m.physical_work = {physical.x, physical.y, physical.width, physical.height - 40 * dpr};
  m.dpr = dpr;
  m.primary = primary;
  return m;
}

TEST(WinScreenLayoutTest, SingleMonitorScalesByDpr) {
  WinScreenLayout layout({Monitor(1, {0, 0, 3840, 2160}, 2.0, true)});
  ASSERT_EQ(layout.monitors().size(), 1u);
  EXPECT_EQ(layout.monitors()[0].logical, (Rect{0, 0, 1920, 1080}));
  EXPECT_EQ(layout.ToLogical(Point{200, 100}), (Point{100, 50}));
  EXPECT_EQ(layout.ToPhysical(Point{100, 50}), (Point{200, 100}));
  EXPECT_EQ(layout.LogicalWorkArea(layout.monitors()[0]), (Rect{0, 0, 1920, 1040}));
  EXPECT_EQ(layout.ToLogical(Rect{100, 200, 400, 300}, 2.0), (Rect{50, 100, 200, 150}));
}

TEST(WinScreenLayoutTest, MixedDpiMonitorsDoNotOverlap) {
  // 主屏 1x 在左, 右侧 2x 4K 屏: 直接按各自 DPR 除会得到 [960, 2880), 与主屏 [0, 1920) 重叠。
  WinScreenLayout layout({Monitor(2, {1920, 0, 3840, 2160}, 2.0), Monitor(1, {0, 0, 1920, 1080}, 1.0, true)});
  ASSERT_EQ(layout.monitors().size(), 2u);
  EXPECT_EQ(layout.monitors()[0].info.id, 1u);  // 主屏在前
  EXPECT_EQ(layout.monitors()[1].logical, (Rect{1920, 0, 1920, 1080}));
  // 右屏上的物理点 ↔ 逻辑点互逆。
  const Point logical = layout.ToLogical(Point{1920 + 400, 300});
  EXPECT_EQ(logical, (Point{2120, 150}));
  EXPECT_EQ(layout.ToPhysical(logical), (Point{2320, 300}));
}

TEST(WinScreenLayoutTest, PlacesMonitorsLeftAndBelowAndChains) {
  // 主屏 2x 居中;左侧 1x;下方 1x;主屏右侧 1x, 其右再接 2x(链式放置)。
  WinScreenLayout layout({
      Monitor(1, {0, 0, 3840, 2160}, 2.0, true),
      Monitor(2, {-1920, 0, 1920, 1080}, 1.0),
      Monitor(3, {0, 2160, 1920, 1080}, 1.0),
      Monitor(4, {3840, 0, 1920, 1080}, 1.0),
      Monitor(5, {5760, 0, 3840, 2160}, 2.0),
  });
  auto logical_of = [&](uint64_t id) {
    for (const auto& m : layout.monitors()) {
      if (m.info.id == id) return m.logical;
    }
    return Rect{};
  };
  EXPECT_EQ(logical_of(1), (Rect{0, 0, 1920, 1080}));
  EXPECT_EQ(logical_of(2), (Rect{-1920, 0, 1920, 1080}));
  EXPECT_EQ(logical_of(3), (Rect{0, 1080, 1920, 1080}));
  EXPECT_EQ(logical_of(4), (Rect{1920, 0, 1920, 1080}));
  EXPECT_EQ(logical_of(5), (Rect{3840, 0, 1920, 1080}));
  // 任意两块逻辑矩形不重叠。
  for (size_t i = 0; i < layout.monitors().size(); ++i) {
    for (size_t j = i + 1; j < layout.monitors().size(); ++j) {
      EXPECT_FALSE(layout.monitors()[i].logical.Intersect(layout.monitors()[j].logical).has_value())
          << i << " vs " << j;
    }
  }
}

TEST(WinScreenLayoutTest, PointsOutsideAllMonitorsUseNearest) {
  WinScreenLayout layout({Monitor(1, {0, 0, 1920, 1080}, 1.5, true)});
  EXPECT_EQ(layout.ToLogical(Point{-30, -15}), (Point{-20, -10}));
  EXPECT_EQ(layout.ToPhysical(Point{-20, -10}), (Point{-30, -15}));
  WinScreenLayout empty;
  EXPECT_EQ(empty.ToLogical(Point{5, 6}), (Point{5, 6}));
}

// ---- 窗口框架 ----

WinFrameState Frame(bool hidden_title_bar) {
  WinFrameState s;
  s.client = {800, 600};
  s.title_bar_hidden = hidden_title_bar;
  s.buttons = LayoutCaptionButtons(800, 32, true);
  return s;
}

TEST(WinFrameTest, CaptionButtonsSitInTheTopRightCorner) {
  const WinCaptionButtonsLayout b = LayoutCaptionButtons(800, 32, false);
  EXPECT_EQ(b.close, (Rect{754, 0, 46, 32}));
  EXPECT_EQ(b.maximize, (Rect{708, 0, 46, 32}));
  EXPECT_EQ(b.minimize, (Rect{662, 0, 46, 32}));
  EXPECT_EQ(b.bounds, (Rect{662, 0, 138, 32}));
  EXPECT_FALSE(b.maximize_enabled);
  EXPECT_TRUE(LayoutCaptionButtons(800, 0, true).empty());
}

TEST(WinFrameTest, ResizeBordersTakePrecedence) {
  WinFrameState s = Frame(true);
  EXPECT_EQ(HitTestFrame(s, {2, 300}), WinFrameHit::kLeft);
  EXPECT_EQ(HitTestFrame(s, {797, 300}), WinFrameHit::kRight);
  EXPECT_EQ(HitTestFrame(s, {400, 597}), WinFrameHit::kBottom);
  EXPECT_EQ(HitTestFrame(s, {400, 2}), WinFrameHit::kTop);
  EXPECT_EQ(HitTestFrame(s, {2, 2}), WinFrameHit::kTopLeft);
  EXPECT_EQ(HitTestFrame(s, {798, 1}), WinFrameHit::kTopRight);  // 最外侧的像素用来缩放
  EXPECT_EQ(HitTestFrame(s, {1, 598}), WinFrameHit::kBottomLeft);
  EXPECT_EQ(HitTestFrame(s, {798, 598}), WinFrameHit::kBottomRight);
  EXPECT_EQ(HitTestFrame(s, {400, 300}), WinFrameHit::kClient);
  // 有系统标题栏时上边不在内容区里缩放。
  EXPECT_EQ(HitTestFrame(Frame(false), {400, 2}), WinFrameHit::kClient);
}

TEST(WinFrameTest, BordersFollowOptionsAndWindowState) {
  WinFrameState s = Frame(true);
  s.resize.border_left = 0;
  EXPECT_EQ(HitTestFrame(s, {0, 300}), WinFrameHit::kClient);
  s.resize.border_bottom = 20;
  EXPECT_EQ(HitTestFrame(s, {400, 585}), WinFrameHit::kBottom);
  s.maximized = true;
  EXPECT_EQ(HitTestFrame(s, {400, 599}), WinFrameHit::kClient);
  s.maximized = false;
  s.resize.resizable = false;
  EXPECT_EQ(HitTestFrame(s, {400, 599}), WinFrameHit::kClient);
}

TEST(WinFrameTest, CaptionButtonsHitOnlyWhenTitleBarHidden) {
  WinFrameState s = Frame(true);
  EXPECT_EQ(HitTestFrame(s, {780, 16}), WinFrameHit::kCloseButton);
  EXPECT_EQ(HitTestFrame(s, {730, 16}), WinFrameHit::kMaximizeButton);
  EXPECT_EQ(HitTestFrame(s, {680, 16}), WinFrameHit::kMinimizeButton);
  EXPECT_EQ(HitTestFrame(s, {650, 16}), WinFrameHit::kClient);
  EXPECT_EQ(HitTestFrame(s, {780, 40}), WinFrameHit::kClient);
  s.buttons = LayoutCaptionButtons(800, 32, false);
  EXPECT_EQ(HitTestFrame(s, {730, 16}), WinFrameHit::kDisabledButton);
  EXPECT_EQ(HitTestFrame(Frame(false), {780, 16}), WinFrameHit::kClient);
  s.fullscreen = true;
  EXPECT_EQ(HitTestFrame(s, {780, 16}), WinFrameHit::kClient);
}

TEST(WinFrameTest, WindowControlsRectReportsButtonArea) {
  EXPECT_EQ(WindowControlsRect(Frame(true)), (std::optional<Rect>(Rect{662, 0, 138, 32})));
  EXPECT_EQ(WindowControlsRect(Frame(false)), std::nullopt);
  WinFrameState full = Frame(true);
  full.fullscreen = true;
  EXPECT_EQ(WindowControlsRect(full), std::nullopt);
}

}  // namespace
}  // namespace momo::embedder::windows
