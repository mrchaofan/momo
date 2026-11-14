// Windows 平台实现单测(依赖 Win32, 只在 Windows 上编译;纯逻辑部分在 win_portable_unittests.cc): WinNativeLoop(投递/延迟/嵌套/Quit)、WinTextInput 的字符提交、
// DwmFlush vsync 生命周期、剪贴板往返。
#include "embedder/windows/win_util.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "base/thread.h"
#include "embedder/common/engine_platform.h"
#include "embedder/windows/win_native_loop.h"
#include "embedder/windows/win_services.h"
#include "embedder/windows/win_text_input.h"
#include "embedder/windows/win_vsync_dwm.h"
#include "gtest/gtest.h"

namespace momo::embedder::windows {
namespace {

// ---- WinNativeLoop ----

TEST(WinNativeLoopTest, RunsTasksInOrderAndQuits) {
  WinNativeLoop loop;
  EXPECT_TRUE(loop.RunsTasksOnCurrentThread());  // Run 之前也正确
  std::vector<int> order;
  loop.PostTask([&] { order.push_back(1); });
  loop.PostDelayedTask([&] { order.push_back(3); }, std::chrono::milliseconds(20));
  loop.PostTask([&] { order.push_back(2); });
  loop.PostDelayedTask(
      [&] {
        order.push_back(4);
        loop.Quit();
      },
      std::chrono::milliseconds(40));
  const TimePoint start = Clock::now();
  loop.Run();
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
  EXPECT_GE(Clock::now() - start, std::chrono::milliseconds(40));
}

TEST(WinNativeLoopTest, CrossThreadPostAndQuit) {
  WinNativeLoop loop;
  std::atomic<bool> off_main{true};
  std::atomic<int> ran{0};
  {
    Thread worker("test.worker");
    worker.GetTaskRunner()->PostTask([&] {
      off_main = !loop.RunsTasksOnCurrentThread();
      for (int i = 0; i < 100; ++i) {
        loop.PostTask([&] { ran++; });
      }
      loop.Quit();  // 他线程 Quit: 投递到主线程执行, 排在前面的任务之后
    });
    loop.Run();
  }
  EXPECT_TRUE(off_main.load());
  EXPECT_EQ(ran.load(), 100);
}

TEST(WinNativeLoopTest, NestedRunQuitsInnermostOnly) {
  WinNativeLoop loop;
  std::vector<std::string> events;
  loop.PostTask([&] {
    events.push_back("outer task");
    loop.PostTask([&] {
      events.push_back("inner task");
      loop.Quit();  // 只结束嵌套的 Run
    });
    loop.Run();
    events.push_back("nested returned");
    loop.PostTask([&] { loop.Quit(); });
  });
  loop.Run();
  EXPECT_EQ(events, (std::vector<std::string>{"outer task", "inner task", "nested returned"}));
}

TEST(WinNativeLoopTest, QuitBeforeRunReturnsImmediately) {
  WinNativeLoop loop;
  loop.Quit();
  loop.Run();  // 不应阻塞
  SUCCEED();
}

TEST(WinNativeLoopTest, DelayedTaskPrecision) {
  WinNativeLoop loop;
  TimePoint fired{};
  const TimePoint start = Clock::now();
  loop.PostDelayedTask(
      [&] {
        fired = Clock::now();
        loop.Quit();
      },
      std::chrono::milliseconds(5));
  loop.Run();
  const auto elapsed = fired - start;
  EXPECT_GE(elapsed, std::chrono::milliseconds(5));
  // 高精度可等待定时器(Win10 1803+)下应远小于系统时钟中断周期(15.6ms)的倍数;留足 CI 余量。
  EXPECT_LT(elapsed, std::chrono::milliseconds(40));
}

// ---- WinTextInput ----

class RecordingDelegate final : public WindowDelegate {
 public:
  void OnMetricsChanged(const platform::ViewportMetrics&) override {}
  void OnPointerPacket(platform::PointerDataPacket) override {}
  void OnKeyData(const platform::KeyData&) override {}
  bool OnCloseRequested() override { return false; }
  void OnFocusChanged(bool) override {}
  void OnWindowStateChanged(const WindowStateFlags&) override {}
  void OnTextInput(const momo::embedder::TextInputEvent& event) override { events.push_back(event); }

  std::vector<momo::embedder::TextInputEvent> events;
};

class WinTextInputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hwnd_ = ::CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr,
                              ::GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(hwnd_, nullptr);
  }
  void TearDown() override { ::DestroyWindow(hwnd_); }

  HWND hwnd_ = nullptr;
  RecordingDelegate delegate_;
};

TEST_F(WinTextInputTest, CharsCommitOnlyWhileShown) {
  WinTextInput input(hwnd_, &delegate_);
  input.OnChar(L'a');
  EXPECT_TRUE(delegate_.events.empty());
  input.Show();
  input.OnChar(L'a');
  input.OnChar(L'\b');  // 控制字符忽略
  input.OnChar(0xD83D);  // 😀 = D83D DE00
  input.OnChar(0xDE00);
  ASSERT_EQ(delegate_.events.size(), 2u);
  EXPECT_EQ(delegate_.events[0].type, momo::embedder::TextInputEvent::Type::kCommit);
  EXPECT_EQ(delegate_.events[0].text, "a");
  EXPECT_EQ(delegate_.events[1].text, "\xF0\x9F\x98\x80");
  input.Hide();
  input.OnChar(L'b');
  EXPECT_EQ(delegate_.events.size(), 2u);
}

TEST_F(WinTextInputTest, ControlCharsAreNotCommitted) {
  // 编辑键产生的 WM_CHAR 控制字符既不提交, 也不转成 kCommand(编辑语义走 platform::KeyData)。
  WinTextInput input(hwnd_, &delegate_);
  input.Show();
  for (wchar_t c : {L'\b', L'\t', L'\r', static_cast<wchar_t>(0x01), static_cast<wchar_t>(0x7F)}) {
    input.OnChar(c);
  }
  EXPECT_TRUE(delegate_.events.empty());
}

// ---- VSync ----

TEST(WinVsyncDwmTest, FiresOnceAndRestarts) {
  momo::testing::ScopedTestEventLoop loop;
  StatusOr<ObjectPtr<platform::VsyncSource>> source = WinVsyncDwm::Create();
  ASSERT_TRUE(source.ok());
  std::atomic<int> fired{0};
  std::atomic<bool> ordered{true};
  std::atomic<bool> on_caller_thread{true};
  const std::thread::id caller = std::this_thread::get_id();
  (*source)->RequestFrameCallback([&](TimePoint start, TimePoint target) {
    if (target <= start) {
      ordered = false;
    }
    // 回调在调用 RequestFrameCallback 的线程执行(vsync 线程只投递)。
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
  ASSERT_EQ(fired.load(), 1);
  for (int i = 0; i < 20; ++i) {
    loop.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(fired.load(), 1);  // 一次性
  EXPECT_TRUE(ordered.load());
  EXPECT_TRUE(on_caller_thread.load());
  (*source)->RequestFrameCallback([&](TimePoint, TimePoint) { fired++; });
  const auto deadline2 = Clock::now() + std::chrono::seconds(2);
  while (fired.load() < 2 && Clock::now() < deadline2) {
    loop.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(fired.load(), 2);
}

TEST(WinVsyncDwmTest, NoCallbackAfterDestruction) {
  momo::testing::ScopedTestEventLoop loop;
  StatusOr<ObjectPtr<platform::VsyncSource>> source = WinVsyncDwm::Create();
  ASSERT_TRUE(source.ok());
  std::atomic<bool> destroyed{false};
  std::atomic<bool> late{false};
  (*source)->RequestFrameCallback([&](TimePoint, TimePoint) {
    if (destroyed.load()) {
      late = true;
    }
  });
  source->reset();      // DeleteLater
  loop.RunUntilIdle();  // 安全点析构(Join vsync 线程)
  destroyed = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(late.load());
}

// ---- 剪贴板 ----

TEST(WinServicesTest, ClipboardRoundTripNormalizesNewlines) {
  WinNativeLoop loop;  // 提供 owner 窗口
  WinServices services(loop.message_window());
  const std::string text = "momo \xE4\xBD\xA0\xE5\xA5\xBD\nline2";
  Status status = services.SetClipboardText(text);
  if (!status.ok()) {
    GTEST_SKIP() << "Clipboard unavailable: " << status;
  }
  StatusOr<std::string> read = services.GetClipboardText();
  ASSERT_TRUE(read.ok()) << read.status();
  EXPECT_EQ(*read, text);
}

TEST(WinServicesTest, OpenUrlRejectsPathsAndFileUrls) {
  WinNativeLoop loop;
  WinServices services(loop.message_window());
  EXPECT_EQ(services.OpenUrl("C:\\Windows\\notepad.exe").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(services.OpenUrl("file:///C:/Windows/notepad.exe").code(),
            StatusCode::kInvalidArgument);
  EXPECT_EQ(services.OpenUrl("no scheme").code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace momo::embedder::windows
