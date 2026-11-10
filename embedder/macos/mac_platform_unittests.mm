// macOS 平台实现单测: 键码映射、IME 状态机(MacTextInput)、CVDisplayLink vsync 生命周期。

#import <Cocoa/Cocoa.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "embedder/common/engine_platform.h"
#include "embedder/macos/mac_key_mapping.h"
#include "embedder/macos/mac_text_input.h"
#include "embedder/macos/mac_vsync_displaylink.h"
#include "gtest/gtest.h"

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
  void OnVisibilityChanged(bool) override {}
  void OnTextInput(const platform::TextInputEvent& event) override { events.push_back(event); }
  std::vector<platform::TextInputEvent> events;
};

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
  EXPECT_EQ(delegate.events[0].type, platform::TextInputEvent::Type::kComposing);
  EXPECT_EQ(delegate.events[0].text, "ni");
  EXPECT_EQ(delegate.events[0].cursor, 2);
  EXPECT_TRUE(input.HasMarkedText());
  EXPECT_EQ(input.MarkedRange().location, 1u);
  EXPECT_EQ(input.MarkedRange().length, 2u);
  EXPECT_EQ(input.SelectedRange().location, 3u);

  // 预编辑被替换为候选词并提交。
  input.InsertText(@"你", NSMakeRange(NSNotFound, 0));
  ASSERT_EQ(delegate.events.size(), 2u);
  EXPECT_EQ(delegate.events[1].type, platform::TextInputEvent::Type::kCommit);
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
  EXPECT_EQ(delegate.events[1].type, platform::TextInputEvent::Type::kComposingEnd);
  EXPECT_FALSE(input.HasMarkedText());

  input.SetMarkedText(@"ka", NSMakeRange(2, 0), NSMakeRange(NSNotFound, 0));
  input.UnmarkText();
  ASSERT_EQ(delegate.events.size(), 4u);
  EXPECT_EQ(delegate.events[3].type, platform::TextInputEvent::Type::kCommit);
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
  EXPECT_EQ(delegate.events[0].type, platform::TextInputEvent::Type::kCommand);
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
