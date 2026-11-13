// macOS 平台实现单测: 键码映射、IME 状态机(MacTextInput)、CVDisplayLink vsync 生命周期。

#import <Cocoa/Cocoa.h>

#include <algorithm>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "embedder/common/engine_platform.h"
#include "embedder/macos/mac_key_mapping.h"
#include "embedder/macos/mac_text_input.h"
#include "embedder/macos/mac_vsync_displaylink.h"
#include "embedder/macos/mac_platform_views.h"
#include "embedder/macos/mac_window.h"
#include "gtest/gtest.h"

#include <string>
@interface RoutedNativeView : NSView
@property(nonatomic) std::vector<std::string>* log;
@end
@implementation RoutedNativeView
- (void)mouseDown:(NSEvent*)e { self.log->push_back("down"); }
- (void)mouseDragged:(NSEvent*)e { self.log->push_back("dragged"); }
- (void)mouseUp:(NSEvent*)e { self.log->push_back("up"); }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }
@end


namespace momo::embedder::macos {
namespace {

// ---- 键码映射 ----

TEST(MacKeyMappingTest, MapsToUsbHidUsages) {
  EXPECT_EQ(MacKeyCodeToHidUsage(0x00), 0x04u);  // A
  EXPECT_EQ(MacKeyCodeToHidUsage(0x06), 0x1Du);  // Z
  EXPECT_EQ(MacKeyCodeToHidUsage(0x1D), 0x27u);  // 0
  EXPECT_EQ(MacKeyCodeToHidUsage(0x24), 0x28u);  // Return
  EXPECT_EQ(MacKeyCodeToHidUsage(0x31), 0x2Cu);  // Space
  EXPECT_EQ(MacKeyCodeToHidUsage(0x37), 0xE3u);  // Command(左)
  EXPECT_EQ(MacKeyCodeToHidUsage(0x36), 0xE7u);  // Command(右)
  EXPECT_EQ(MacKeyCodeToHidUsage(0x7A), 0x3Au);  // F1
  EXPECT_EQ(MacKeyCodeToHidUsage(0x7E), 0x52u);  // ArrowUp
  EXPECT_EQ(MacKeyCodeToHidUsage(0x52), 0x62u);  // Numpad0
  EXPECT_EQ(MacKeyCodeToHidUsage(0x3F), 0u);     // Fn: 无 usage
  EXPECT_EQ(MacKeyCodeToHidUsage(0xFF), 0u);
  EXPECT_EQ(PhysicalKeyFromHidUsage(0x04), 0x00070004u);
  EXPECT_EQ(PhysicalKeyFromHidUsage(0), 0u);
}

TEST(MacKeyMappingTest, ClassifiesSpecialKeys) {
  EXPECT_TRUE(IsSpecialKeyCode(0x24));   // Return
  EXPECT_TRUE(IsSpecialKeyCode(0x35));   // Escape
  EXPECT_TRUE(IsSpecialKeyCode(0x38));   // Shift
  EXPECT_TRUE(IsSpecialKeyCode(0x7B));   // ArrowLeft
  EXPECT_TRUE(IsSpecialKeyCode(0x7A));   // F1
  EXPECT_TRUE(IsSpecialKeyCode(0x69));   // F13
  EXPECT_FALSE(IsSpecialKeyCode(0x00));  // A
  EXPECT_FALSE(IsSpecialKeyCode(0x31));  // Space(可打印)
  EXPECT_FALSE(IsSpecialKeyCode(0x52));  // Numpad0
  EXPECT_FALSE(IsSpecialKeyCode(0x51));  // NumpadEqual
  EXPECT_EQ(DeviceModifierMaskForKeyCode(0x38), 0x2u);
  EXPECT_EQ(DeviceModifierMaskForKeyCode(0x00), 0u);
}

// ---- IME 状态机 ----

class RecordingDelegate final : public WindowDelegate {
 public:
  void OnMetricsChanged(const platform::ViewportMetrics&) override {}
  void OnPointerPacket(platform::PointerDataPacket) override {}
  void OnKeyData(const platform::KeyData&) override {}
  bool OnCloseRequested() override { return true; }
  void OnFocusChanged(bool) override {}
  void OnWindowStateChanged(const momo::embedder::WindowStateFlags& s) override { states.push_back(s); }
  std::vector<momo::embedder::WindowStateFlags> states;
  void OnTextInput(const momo::embedder::TextInputEvent& event) override { events.push_back(event); }
  std::vector<momo::embedder::TextInputEvent> events;
};

// ---- 平台视图输入(主线程按每帧几何同步路由)----

// ---- 指针(MacWindow ↔ PointerDispatcher 的接入)----
// 接收方的规则由 PointerDispatcher 的单测覆盖(embedder/common/pointer_dispatcher_unittests.cc);这里验证
// macOS 的接入: NSEvent 的翻译、窗口归属(含不属于任何窗口的事件)、按帧几何命中平台视图、原生投递。

class PointerRecordingDelegate final : public WindowDelegate {
 public:
  void OnMetricsChanged(const platform::ViewportMetrics&) override {}
  void OnPointerPacket(platform::PointerDataPacket packet) override {
    for (const platform::PointerData& p : packet) {
      if (p.phase != platform::PointerPhase::kAdd) phases.push_back(p.phase);
      if (p.sequence_id != 0) sequence_id = p.sequence_id;
    }
  }
  void OnKeyData(const platform::KeyData&) override {}
  bool OnCloseRequested() override { return true; }
  void OnFocusChanged(bool) override {}
  void OnWindowStateChanged(const momo::embedder::WindowStateFlags& s) override { states.push_back(s); }
  std::vector<momo::embedder::WindowStateFlags> states;
  void OnTextInput(const momo::embedder::TextInputEvent&) override {}
  std::vector<platform::PointerPhase> phases;
  int64_t sequence_id = 0;
};

using Phase = platform::PointerPhase;
using Phases = std::vector<Phase>;

// CoreGraphics 的未公开函数(仅测试使用): 设置事件的窗口内位置。
extern "C" void CGEventSetWindowLocation(CGEventRef event, CGPoint location);

class MacPointerSequenceTest : public ::testing::Test {
 protected:
  void SetUp() override { [NSApplication sharedApplication]; }
  // 屏幕上互不重叠的窗口: AppKit 只为屏幕上的窗口投递鼠标事件, 命中也只认最上层的窗口。
  std::unique_ptr<MacWindow> Window(PointerRecordingDelegate* delegate) {
    momo::embedder::WindowSpec spec;
    spec.size = {200, 100};
    auto window = std::make_unique<MacWindow>(spec, delegate);
    static int count = 0;
    const NSRect screen = NSScreen.mainScreen.visibleFrame;
    [window->ns_window() setFrameTopLeftPoint:NSMakePoint(NSMinX(screen) + 250 * (count++ % 4), NSMaxY(screen))];
    [window->ns_window() orderFrontRegardless];
    return window;
  }
  // 屏幕位置 screen(主屏左上原点)处的鼠标事件, 属于 window_number 的窗口(已关闭的窗口编号: 不属于任何窗口)。
  // 经 CGEvent 构造: 事件的屏幕位置与窗口内位置一致(宿主按屏幕位置判定接收方)。
  static NSEvent* Mouse(NSEventType type, NSPoint screen, NSInteger window_number) {
    CGEventRef cg = CGEventCreateMouseEvent(nullptr, static_cast<CGEventType>(type), screen, kCGMouseButtonLeft);
    CGEventSetIntegerValueField(cg, kCGMouseEventWindowUnderMousePointer, window_number);
    CGEventSetIntegerValueField(cg, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, window_number);
    CGEventSetIntegerValueField(cg, static_cast<CGEventField>(51), window_number);  // NSEvent.windowNumber
    if (NSWindow* window = [NSApp windowWithWindowNumber:window_number]) {
      // 窗口内位置(左上原点): NSEvent.locationInWindow 由它换算。
      const NSPoint cocoa = NSMakePoint(screen.x, NSMaxY(NSScreen.screens.firstObject.frame) - screen.y);
      const NSPoint p = [window convertPointFromScreen:cocoa];
      CGEventSetWindowLocation(cg, CGPointMake(p.x, NSHeight(window.frame) - p.y));
    }
    NSEvent* event = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    return event;
  }
  // 窗口内容区中央(避开边缘缩放区)。
  static NSPoint Center(MacWindow& window) {
    const momo::Rect f = window.screen_frame();
    return NSMakePoint(f.x + f.width / 2, f.y + f.height / 2);
  }
  static NSEvent* Mouse(NSEventType type, NSInteger window_number) {
    return Mouse(type, NSMakePoint(0, 0), window_number);
  }
  static void Send(MacWindow& window, NSEventType type) {
    [window.ns_window() sendEvent:Mouse(type, Center(window), window.ns_window().windowNumber)];
  }
};

// 原生事件落在平台视图上(且没被 momo 内容盖住): 主线程直接交给原生视图, momo 收不到;盖住的部分归 momo。
// 按下时决定的接收方固定到抬起, 即使拖动中几何变化。
// 窗口状态: 显示/隐藏/缩放/还原经 OnWindowStateChanged 上报(最小化依赖窗口服务器的动画, 不在这里测)。
TEST_F(MacPointerSequenceTest, WindowStateChangesAreReported) {
  if (NSScreen.mainScreen == nil) GTEST_SKIP() << "No display";
  PointerRecordingDelegate delegate;
  momo::embedder::WindowSpec spec;
  spec.size = {200, 100};
  MacWindow window(spec, &delegate);
  EXPECT_FALSE(window.state().visible) << "not shown yet";
  window.Show();
  ASSERT_FALSE(delegate.states.empty());
  EXPECT_TRUE(delegate.states.back().visible);
  ASSERT_TRUE(window.Hide().ok());
  EXPECT_FALSE(delegate.states.back().visible);
  EXPECT_FALSE(window.state().shown());
  window.Show();
  ASSERT_TRUE(window.Maximize().ok());
  EXPECT_TRUE(window.state().maximized);
  ASSERT_TRUE(window.Restore().ok());
  EXPECT_FALSE(window.state().maximized);
  EXPECT_TRUE(delegate.states.back().visible);
}

TEST_F(MacPointerSequenceTest, PlatformViewInputIsRoutedOnTheMainThreadByFrameGeometry) {
  PointerRecordingDelegate delegate;
  auto window = Window(&delegate);
  std::vector<std::string> log;
  RoutedNativeView* native = [[RoutedNativeView alloc] initWithFrame:NSMakeRect(0, 0, 10, 10)];
  native.log = &log;
  window->AddPlatformView(9, (__bridge void*)native);
  const double scale = window->device_pixel_ratio();
  auto apply = [&](std::vector<momo::Rect> occluders) {
    momo::engine::platform::CompositionLayout layout;
    momo::engine::platform::PlatformViewGeometry g;
    g.view_id = 9;
    g.frame = {0, 0, 200 * scale, 100 * scale};  // 铺满内容区
    g.occluders = std::move(occluders);
    layout.views.push_back(g);
    layout.overlays.push_back(std::nullopt);
    [window->platform_views() applyLayout:layout overlayLayers:{nil}];
  };
  apply({});
  Send(*window, NSEventTypeLeftMouseDown);
  apply({{0, 0, 200 * scale, 100 * scale}});  // 拖动中 momo 内容盖上来: 接收方不变
  Send(*window, NSEventTypeLeftMouseDragged);
  Send(*window, NSEventTypeLeftMouseUp);
  EXPECT_EQ(log, (std::vector<std::string>{"down", "dragged", "up"}));
  EXPECT_EQ(std::count(delegate.phases.begin(), delegate.phases.end(), Phase::kDown), 0)
      << "momo does not see the platform view's sequence";

  log.clear();
  Send(*window, NSEventTypeLeftMouseDown);  // 现在被 momo 内容盖住: 归 momo
  Send(*window, NSEventTypeLeftMouseUp);
  EXPECT_EQ(log, std::vector<std::string>{});
  EXPECT_EQ(std::count(delegate.phases.begin(), delegate.phases.end(), Phase::kDown), 1);
  window->RemovePlatformView(9);
}

// 拖动中并入另一个窗口: 序列改交目标窗口, 按下所在的窗口随后关闭, 拖动照常继续到抬起。
TEST_F(MacPointerSequenceTest, CapturedSequenceContinuesAfterTheOriginWindowCloses) {
  PointerRecordingDelegate da, db;
  auto a = Window(&da);
  auto b = Window(&db);
  Send(*a, NSEventTypeLeftMouseDown);
  EXPECT_EQ(da.phases, (Phases{Phase::kDown}));
  EXPECT_EQ(b->CapturePointer(da.sequence_id + 1).value(), false) << "another sequence";
  ASSERT_EQ(b->CapturePointer(da.sequence_id).value(), true);
  EXPECT_EQ(db.phases, Phases{}) << "capturing synthesizes nothing";

  Send(*a, NSEventTypeLeftMouseDragged);  // AppKit 把拖动投递给按下所在的窗口
  EXPECT_EQ(da.phases, (Phases{Phase::kDown})) << "the origin no longer reports the pointer";
  EXPECT_EQ(db.phases, (Phases{Phase::kMove})) << "the receiver does (with buttons held)";

  const NSInteger origin = a->ns_window().windowNumber;
  a->ReleasePointerCapture();  // 关窗前交出捕获: 它已不是接收方, 什么也不做
  a.reset();
  EXPECT_TRUE(MomoRoutePointerSequenceEvent(Mouse(NSEventTypeLeftMouseDragged, Center(*b), origin)));
  EXPECT_TRUE(MomoRoutePointerSequenceEvent(Mouse(NSEventTypeLeftMouseUp, Center(*b), origin)));
  EXPECT_EQ(db.phases, (Phases{Phase::kMove, Phase::kMove, Phase::kUp}));
  EXPECT_FALSE(MomoRoutePointerSequenceEvent(Mouse(NSEventTypeLeftMouseDragged, Center(*b), origin)))
      << "the sequence ended with the release";
}

// 序列期间, 非接收窗口收到的按键/拖动转给接收方, 自己不上报这个指针(接收方固定)。
TEST_F(MacPointerSequenceTest, OtherWindowsDoNotReportThePointerDuringASequence) {
  PointerRecordingDelegate da, db;
  auto a = Window(&da);
  auto b = Window(&db);
  Send(*a, NSEventTypeLeftMouseDown);
  Send(*b, NSEventTypeLeftMouseDragged);
  Send(*b, NSEventTypeLeftMouseUp);
  EXPECT_EQ(db.phases, Phases{});
  EXPECT_EQ(da.phases, (Phases{Phase::kDown, Phase::kMove, Phase::kUp, Phase::kRemove}))
      << "released over the other window: the pointer left";
  Send(*b, NSEventTypeMouseMoved);  // 序列结束: 照常上报
  EXPECT_EQ(db.phases, (Phases{Phase::kHover}));
}

// 抬起被嵌套事件循环吞掉(系统层已没有按键按住): 下一个无按键的移动或新的按下即结束旧序列——接收方补报抬起,
// 本事件照常。即使接收窗口已关闭, 新的按下也不会被当作旧序列丢弃。
TEST_F(MacPointerSequenceTest, AStaleSequenceEndsOnTheNextMoveOrPress) {
  PointerRecordingDelegate da, db;
  auto a = Window(&da);
  auto b = Window(&db);
  Send(*a, NSEventTypeLeftMouseDown);  // 测试环境里系统层没有按键按住: 等同于抬起被吞掉
  Send(*b, NSEventTypeMouseMoved);
  EXPECT_EQ(da.phases, (Phases{Phase::kDown, Phase::kUp, Phase::kRemove}))
      << "the receiver reports the missed release; the pointer is now over the other window";
  EXPECT_EQ(db.phases, (Phases{Phase::kHover}));

  Send(*a, NSEventTypeLeftMouseDown);
  a->ReleasePointerCapture();
  a.reset();  // 接收窗口关闭, 抬起也没见到
  Send(*b, NSEventTypeLeftMouseDown);
  EXPECT_EQ(db.phases.back(), Phase::kDown) << "a new press starts a new sequence";
  Send(*b, NSEventTypeLeftMouseUp);
}

// 接收窗口关闭: 序列以取消结束(kCancel), 之后到抬起为止的事件丢弃。
TEST_F(MacPointerSequenceTest, ClosingTheReceiverCancelsTheSequence) {
  PointerRecordingDelegate da;
  auto a = Window(&da);
  const NSInteger number = a->ns_window().windowNumber;
  Send(*a, NSEventTypeLeftMouseDown);
  a->ReleasePointerCapture();
  EXPECT_EQ(da.phases, (Phases{Phase::kDown, Phase::kCancel}));
  a.reset();
  EXPECT_TRUE(MomoRoutePointerSequenceEvent(Mouse(NSEventTypeLeftMouseDragged, number))) << "dropped";
  EXPECT_TRUE(MomoRoutePointerSequenceEvent(Mouse(NSEventTypeLeftMouseUp, number))) << "dropped; ends";
  EXPECT_FALSE(MomoRoutePointerSequenceEvent(Mouse(NSEventTypeLeftMouseDragged, number)));
}

// 没有进行中的序列(已抬起): 改交什么也不做, 抬起已由原接收窗口上报。
TEST_F(MacPointerSequenceTest, CapturingWithoutASequenceIsANoop) {
  PointerRecordingDelegate da;
  auto a = Window(&da);
  EXPECT_EQ(a->CapturePointer(1).value(), false);
  EXPECT_EQ(da.phases, Phases{});
}

TEST(MacTextInputTest, IgnoresInputWhenHidden) {
  RecordingDelegate delegate;
  MacTextInput input(nil, &delegate);
  input.InsertText(@"a", NSMakeRange(NSNotFound, 0));
  input.DoCommand(@selector(deleteBackward:));
  EXPECT_TRUE(delegate.events.empty());
  EXPECT_FALSE(input.active());
}

TEST(MacTextInputTest, CompositionThenCommit) {
  RecordingDelegate delegate;
  MacTextInput input(nil, &delegate);
  input.Show();
  input.SetEditingState({"ab", 1, 1, -1, -1});  // a|b
  EXPECT_EQ(input.SelectedRange().location, 1u);
  EXPECT_FALSE(input.HasMarkedText());

  input.SetMarkedText(@"ni", NSMakeRange(2, 0), NSMakeRange(NSNotFound, 0));
  ASSERT_EQ(delegate.events.size(), 1u);
  EXPECT_EQ(delegate.events[0].type, momo::embedder::TextInputEvent::Type::kComposing);
  EXPECT_EQ(delegate.events[0].text, "ni");
  EXPECT_EQ(delegate.events[0].cursor, 2);
  EXPECT_TRUE(input.HasMarkedText());
  EXPECT_EQ(input.MarkedRange().location, 1u);
  EXPECT_EQ(input.MarkedRange().length, 2u);
  EXPECT_EQ(input.SelectedRange().location, 3u);

  // 预编辑被替换为候选词并提交。
  input.InsertText(@"你", NSMakeRange(NSNotFound, 0));
  ASSERT_EQ(delegate.events.size(), 2u);
  EXPECT_EQ(delegate.events[1].type, momo::embedder::TextInputEvent::Type::kCommit);
  EXPECT_EQ(delegate.events[1].text, "你");
  EXPECT_FALSE(input.HasMarkedText());
  EXPECT_EQ(input.SelectedRange().location, 2u);
  NSAttributedString* all = input.AttributedSubstring(NSMakeRange(0, 10), nullptr);
  EXPECT_TRUE([all.string isEqualToString:@"a你b"]);
}

TEST(MacTextInputTest, EmptyMarkedTextEndsComposingAndUnmarkCommits) {
  RecordingDelegate delegate;
  MacTextInput input(nil, &delegate);
  input.Show();
  input.SetMarkedText(@"x", NSMakeRange(1, 0), NSMakeRange(NSNotFound, 0));
  input.SetMarkedText(@"", NSMakeRange(0, 0), NSMakeRange(NSNotFound, 0));
  ASSERT_EQ(delegate.events.size(), 2u);
  EXPECT_EQ(delegate.events[1].type, momo::embedder::TextInputEvent::Type::kComposingEnd);
  EXPECT_FALSE(input.HasMarkedText());

  input.SetMarkedText(@"ka", NSMakeRange(2, 0), NSMakeRange(NSNotFound, 0));
  input.UnmarkText();
  ASSERT_EQ(delegate.events.size(), 4u);
  EXPECT_EQ(delegate.events[3].type, momo::embedder::TextInputEvent::Type::kCommit);
  EXPECT_EQ(delegate.events[3].text, "ka");
  EXPECT_FALSE(input.HasMarkedText());
}

TEST(MacTextInputTest, CommandsDropTrailingColonAndNoop) {
  RecordingDelegate delegate;
  MacTextInput input(nil, &delegate);
  input.Show();
  input.DoCommand(@selector(insertNewline:));
  input.DoCommand(@selector(noop:));
  input.DoCommand(@selector(moveToBeginningOfLine:));
  ASSERT_EQ(delegate.events.size(), 2u);
  EXPECT_EQ(delegate.events[0].type, momo::embedder::TextInputEvent::Type::kCommand);
  EXPECT_EQ(delegate.events[0].command, "insertNewline");
  EXPECT_EQ(delegate.events[1].command, "moveToBeginningOfLine");
}

TEST(MacTextInputTest, HideDiscardsCompositionSilently) {
  RecordingDelegate delegate;
  MacTextInput input(nil, &delegate);
  input.Show();
  input.SetMarkedText(@"zh", NSMakeRange(2, 0), NSMakeRange(NSNotFound, 0));
  input.Hide();
  EXPECT_FALSE(input.HasMarkedText());
  input.InsertText(@"q", NSMakeRange(NSNotFound, 0));
  EXPECT_EQ(delegate.events.size(), 1u);  // 只有 Hide 前的 composing
}

TEST(MacTextInputTest, EditingStateUsesUtf16Indices) {
  RecordingDelegate delegate;
  MacTextInput input(nil, &delegate);
  input.Show();
  // "😀a": 😀 占 2 个 UTF-16 码元。
  input.SetEditingState({"\xF0\x9F\x98\x80" "a", 2, 3, 2, 3});
  EXPECT_EQ(input.SelectedRange().location, 2u);
  EXPECT_EQ(input.SelectedRange().length, 1u);
  EXPECT_EQ(input.MarkedRange().location, 2u);
  EXPECT_TRUE([input.AttributedSubstring(input.MarkedRange(), nullptr).string isEqualToString:@"a"]);
  // 越界下标被夹紧。
  input.SetEditingState({"ab", 99, -5, -1, -1});
  EXPECT_EQ(input.SelectedRange().location, 0u);
  EXPECT_EQ(input.SelectedRange().length, 2u);
}

// ---- VSync ----

TEST(MacVsyncDisplayLinkTest, FiresOnceAndStopsWhenIdle) {
  momo::testing::ScopedTestEventLoop loop;
  StatusOr<ObjectPtr<platform::VsyncSource>> source = MacVsyncDisplayLink::Create(nil);
  if (!source.ok()) {
    GTEST_SKIP() << "No display link: " << source.status();
  }
  std::atomic<int> fired{0};
  std::atomic<bool> ordered{true};
  std::atomic<bool> on_caller_thread{true};
  const std::thread::id caller = std::this_thread::get_id();
  (*source)->RequestFrameCallback([&](TimePoint start, TimePoint target) {
    if (target <= start) {
      ordered = false;
    }
    // 回调必须在调用 RequestFrameCallback 的线程执行, 而不是 CVDisplayLink 线程。
    if (std::this_thread::get_id() != caller) {
      on_caller_thread = false;
    }
    fired++;
  });
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (fired.load() == 0 && Clock::now() < deadline) {
    loop.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (fired.load() == 0) {
    GTEST_SKIP() << "Display link did not tick (headless environment?)";
  }
  // 一次性: 再等几帧也不会重复触发;空闲 tick 在本线程停止 display link。
  for (int i = 0; i < 20; ++i) {
    loop.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(fired.load(), 1);
  EXPECT_TRUE(ordered.load());
  EXPECT_TRUE(on_caller_thread.load());

  // 停止后可再次请求。
  (*source)->RequestFrameCallback([&](TimePoint, TimePoint) { fired++; });
  const auto deadline2 = Clock::now() + std::chrono::seconds(2);
  while (fired.load() < 2 && Clock::now() < deadline2) {
    loop.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(fired.load(), 2);
}

TEST(MacVsyncDisplayLinkTest, NoCallbackAfterDestruction) {
  momo::testing::ScopedTestEventLoop loop;
  StatusOr<ObjectPtr<platform::VsyncSource>> source = MacVsyncDisplayLink::Create(nil);
  if (!source.ok()) {
    GTEST_SKIP() << "No display link: " << source.status();
  }
  std::atomic<bool> destroyed{false};
  std::atomic<bool> late{false};
  (*source)->RequestFrameCallback([&](TimePoint, TimePoint) {
    if (destroyed.load()) {
      late = true;
    }
  });
  source->reset();       // DeleteLater
  loop.RunUntilIdle();   // 安全点析构
  destroyed = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  loop.RunUntilIdle();   // 析构前已投递的 tick 必须作废
  EXPECT_FALSE(late.load());
}

}  // namespace
}  // namespace momo::embedder::macos
