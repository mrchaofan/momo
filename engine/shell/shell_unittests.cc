#include "engine/shell/shell.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <vector>
#include <memory>
#include <string>

#include "base/event_loop.h"
#include "base/waitable_event.h"
#include "engine/common/standard_codec.h"
#include "engine/shell/testing/shell_test_utils.h"
#include "engine/shell/thread_host.h"
#include "gtest/gtest.h"

namespace momo::engine {
namespace {

using testing::FakeSurface;
using testing::FakeVsyncSource;
using testing::WaitFor;

class RecordingShellDelegate final : public Shell::Delegate {
 public:
  void* GetNativeWindowHandle() override { return nullptr; }
  void OnFirstFramePresented() override { first_frames++; }
  int first_frames = 0;
};

// 宿主通道(on_created 注册): 记录 JS 发来的方法调用(channel:method)。
std::function<void(Shell&)> RecordCalls(const std::string& channel,
                                        std::vector<std::string>* calls,
                                        std::shared_ptr<TaskRunner> runner) {
  return [channel, calls, runner](Shell& shell) {
    shell.hub()->SetNativeHandler(channel, runner, [calls](MessageBytes message, MessageReply reply) {
      StatusOr<MethodCall> call = DecodeMethodCall(message);
      calls->push_back(call.ok() ? call->method + ":" + call->args.Find("title")->AsString().value_or("")
                                 : "<bad>");
      reply.Send(EncodeSuccessEnvelope(EncodableValue()));
    });
  };
}

// platform 线程 = 测试主线程, 绑定一个 EventLoop;需要时由 Pump()/WaitFor 驱动。
class ShellTest : public ::testing::Test {
 protected:
  ShellTest() : platform_loop_(MakeBoundLoop()), threads_("test"),
                runners_(threads_.MakeTaskRunners(platform_loop_->GetTaskRunner())) {}
  ~ShellTest() override {
    // 结束 platform 循环: 排空任务并清理 DeleteLater 对象。
    platform_loop_->Terminate();
    platform_loop_->RunUntilIdle();
  }

  static std::shared_ptr<EventLoop> MakeBoundLoop() {
    auto loop = EventLoop::Create();
    loop->BindToCurrentThread();
    return loop;
  }

  void Pump() { platform_loop_->RunUntilIdle(); }
  bool PumpUntil(const std::function<bool()>& condition) {
    return WaitFor(condition, std::chrono::milliseconds(3000), platform_loop_.get());
  }

  static platform::ViewportMetrics Metrics(double w, double h) { return {{w, h}, 2.0}; }

  std::shared_ptr<EventLoop> platform_loop_;
  ThreadHost threads_;
  TaskRunners runners_;
};

TEST_F(ShellTest, CreateWithoutBundleSucceedsAndShutsDown) {
  auto shell = Shell::Create(runners_, Settings{});
  ASSERT_TRUE(shell.ok()) << shell.status();
  (*shell)->Shutdown();
  EXPECT_TRUE((*shell)->is_shut_down());
  (*shell)->Shutdown();  // 幂等
}

TEST_F(ShellTest, CreateFailsForMissingBundle) {
  Settings settings;
  settings.bundle_path = "/definitely/not/here/bundle.js";
  auto shell = Shell::Create(runners_, settings);
  EXPECT_EQ(shell.status().code(), StatusCode::kNotFound);
}

TEST_F(ShellTest, CreateRejectsIncompleteRunners) {
  TaskRunners partial = runners_;
  partial.io = nullptr;
  EXPECT_EQ(Shell::Create(partial, {}).status().code(), StatusCode::kInvalidArgument);
}

TEST_F(ShellTest, MetricsChangeProducesFrameWithTimerVsync) {
  Settings settings;
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok());
  FakeSurface surface;
  RecordingShellDelegate delegate;
  (*shell)->AttachView(&delegate, &surface);  // 无平台 vsync: 定时器回退

  (*shell)->OnMetricsChanged(Metrics(800, 600));
  EXPECT_TRUE(WaitFor([&] { return surface.presented.load() >= 1; }));
  EXPECT_EQ(surface.last_width.load(), 800);

  // 相同 metrics 不再出帧;新尺寸再出一帧。
  (*shell)->OnMetricsChanged(Metrics(800, 600));
  (*shell)->OnMetricsChanged(Metrics(1024, 600));
  EXPECT_TRUE(WaitFor([&] { return surface.last_width.load() == 1024; }));

  (*shell)->DetachView();
  const int presented = surface.presented.load();
  (*shell)->OnMetricsChanged(Metrics(640, 480));  // 解绑后不再触碰 surface
  (*shell)->Shutdown();
  EXPECT_EQ(surface.presented.load(), presented);
}

TEST_F(ShellTest, PlatformVsyncSourceDrivesFramesAndIsDeletedLaterOnDetach) {
  auto shell = Shell::Create(runners_, Settings{});
  ASSERT_TRUE(shell.ok());
  FakeSurface surface;
  RecordingShellDelegate delegate;
  int destroyed = 0;
  bool destroyed_on_platform = false;
  auto source = MakeObject<FakeVsyncSource>([&] {
    destroyed++;
    destroyed_on_platform = runners_.platform->RunsTasksOnCurrentThread();
  });
  FakeVsyncSource* raw_source = source.get();
  (*shell)->AttachView(&delegate, &surface, std::move(source));

  (*shell)->OnMetricsChanged(Metrics(320, 240));
  // vsync 请求经 platform 线程转发: 需要驱动 platform 循环。
  EXPECT_TRUE(PumpUntil([&] { return surface.presented.load() >= 1; }));
  EXPECT_GE(raw_source->requests.load(), 1);
  EXPECT_GE(raw_source->callbacks_fired.load(), 1);
  EXPECT_FALSE(raw_source->called_off_thread.load());
  // 回调在调用方(platform)线程执行, 由 PlatformVsyncWaiter 再投递回 ui 线程。
  EXPECT_FALSE(raw_source->callback_off_thread.load());

  // 在 platform 任务中解绑: VsyncSource 不在调用栈内析构, 而是在本轮安全点。
  int destroyed_inside_task = -1;
  runners_.platform->PostTask([&] {
    (*shell)->DetachView();
    destroyed_inside_task = destroyed;
  });
  Pump();
  EXPECT_EQ(destroyed_inside_task, 0);
  EXPECT_EQ(destroyed, 1);
  EXPECT_TRUE(destroyed_on_platform);
  (*shell)->Shutdown();
}

TEST_F(ShellTest, DetachAfterSourceGoneFallsBackToTimer) {
  Settings settings;
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok());
  FakeSurface surface;
  RecordingShellDelegate delegate;
  (*shell)->AttachView(&delegate, &surface, MakeObject<FakeVsyncSource>());
  (*shell)->DetachView();
  Pump();  // source 析构
  // 重新绑定(无平台 vsync): 帧由定时器驱动, 不依赖已析构的 source。
  (*shell)->AttachView(&delegate, &surface);
  (*shell)->OnMetricsChanged(Metrics(64, 64));
  EXPECT_TRUE(PumpUntil([&] { return surface.presented.load() >= 1; }));
  (*shell)->Shutdown();
}

TEST_F(ShellTest, SuspendedAppStopsFramesUntilResumed) {
  Settings settings;
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok());
  FakeSurface surface;
  RecordingShellDelegate delegate;
  (*shell)->AttachView(&delegate, &surface);

  (*shell)->SetAppSuspended(true);
  (*shell)->OnMetricsChanged(Metrics(500, 500));
  EXPECT_FALSE(WaitFor([&] { return surface.acquired.load() > 0; },
                       std::chrono::milliseconds(60)));

  (*shell)->SetAppSuspended(false);
  EXPECT_TRUE(WaitFor([&] { return surface.presented.load() >= 1; }));
  (*shell)->Shutdown();
}

// 视图可见性按视图: 隐藏的视图不出帧, 其他视图照常;全部隐藏时帧时钟停止;重新可见时补一帧。
TEST_F(ShellTest, HiddenViewIsNotDrawnWhileOthersAre) {
  Settings settings;
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok());
  FakeSurface a_surface, b_surface;
  RecordingShellDelegate a, b;
  (*shell)->AttachView(0, &a, &a_surface);
  (*shell)->AttachView(1, &b, &b_surface);
  (*shell)->OnViewVisibilityChanged(1, false);
  (*shell)->OnMetricsChanged(0, Metrics(100, 100));
  (*shell)->OnMetricsChanged(1, Metrics(100, 100));
  EXPECT_TRUE(WaitFor([&] { return a_surface.presented.load() >= 1; }));
  EXPECT_FALSE(WaitFor([&] { return b_surface.acquired.load() > 0; }, std::chrono::milliseconds(60)))
      << "a hidden view is not drawn";

  (*shell)->OnViewVisibilityChanged(0, false);  // 全部隐藏: 停帧
  const int a_before = a_surface.acquired.load();
  (*shell)->OnMetricsChanged(0, Metrics(200, 100));
  EXPECT_FALSE(WaitFor([&] { return a_surface.acquired.load() > a_before; },
                       std::chrono::milliseconds(60)));

  (*shell)->OnViewVisibilityChanged(1, true);  // 重新可见: 补一帧
  EXPECT_TRUE(WaitFor([&] { return b_surface.presented.load() >= 1; }));
  (*shell)->Shutdown();
}

// 每次绑定后第一帧呈现时通知视图的宿主(show:false 窗口据此显示)。
TEST_F(ShellTest, FirstPresentedFrameIsReportedOncePerAttach) {
  Settings settings;
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok());
  FakeSurface surface;
  RecordingShellDelegate delegate;
  (*shell)->AttachView(&delegate, &surface);
  (*shell)->OnMetricsChanged(Metrics(64, 64));
  EXPECT_TRUE(PumpUntil([&] { return delegate.first_frames == 1; }));
  (*shell)->OnMetricsChanged(Metrics(128, 64));
  EXPECT_TRUE(PumpUntil([&] { return surface.presented.load() >= 2; }));
  Pump();
  EXPECT_EQ(delegate.first_frames, 1);
  (*shell)->Shutdown();
}

// 宿主不建启动窗口: 引擎里没有隐式视图, 视图全部来自之后绑定的窗口。
TEST_F(ShellTest, NoImplicitViewUntilTheHostAttachesOne) {
  Settings settings;
  settings.implicit_view = false;
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok());
  FakeSurface surface;
  RecordingShellDelegate delegate;
  (*shell)->AttachView(4, &delegate, &surface);
  (*shell)->OnMetricsChanged(4, Metrics(64, 64));
  EXPECT_TRUE(WaitFor([&] { return surface.presented.load() >= 1; }));
  (*shell)->Shutdown();
}

#if MOMO_HAS_JS_RUNTIME

int Channel(uint32_t argb, int shift) { return static_cast<int>((argb >> shift) & 0xFF); }

// 端到端: 真实示例 bundle → JS 运行时 → 帧管线 → 软件光栅 → surface 像素。
// process.exit(code) 不直接退出进程: 停止 JS 并在 platform 线程把退出码交给宿主。
TEST_F(ShellTest, ProcessExitIsHandedToTheHost) {
  const std::filesystem::path bundle =
      std::filesystem::temp_directory_path() / "momo_shell_process_exit.mjs";
  std::ofstream(bundle) << "setTimeout(() => { process.exit(3); globalThis.after = true; }, 0);\n";
  Settings settings;
  settings.bundle_path = bundle;
  std::optional<int> code;
  auto shell = Shell::Create(runners_, settings, nullptr, [&](int c) { code = c; });
  ASSERT_TRUE(shell.ok()) << shell.status();
  EXPECT_TRUE(PumpUntil([&] { return code.has_value(); }));
  EXPECT_EQ(code.value_or(-1), 3);
  (*shell)->Shutdown();
  std::filesystem::remove(bundle);
}

TEST_F(ShellTest, HelloFrameExampleRendersPixels) {
  Settings settings;
  settings.bundle_path = std::filesystem::path(MOMO_SOURCE_DIR) / "examples/hello_frame/main.js";
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok()) << shell.status();
  FakeSurface surface;
  RecordingShellDelegate delegate;
  (*shell)->AttachView(&delegate, &surface);
  (*shell)->OnMetricsChanged(Metrics(800, 600));  // dpr 2: 逻辑 400x300

  ASSERT_TRUE(PumpUntil([&] { return surface.presented.load() >= 3; })) << "example keeps animating";

  // 背景 0xF5F5F7(物理 (10,10) = 逻辑 (5,5))。
  const uint32_t bg = surface.PresentedPixel(10, 10);
  EXPECT_EQ(bg, 0xFFF5F5F7u) << std::hex << bg;
  // 卡片渐变的中点(逻辑 (200,120))为中间色 0x9C27B0。
  const uint32_t mid = surface.PresentedPixel(400, 240);
  EXPECT_NEAR(Channel(mid, 16), 0x9C, 6) << std::hex << mid;
  EXPECT_NEAR(Channel(mid, 8), 0x27, 6) << std::hex << mid;
  EXPECT_NEAR(Channel(mid, 0), 0xB0, 6) << std::hex << mid;
  (*shell)->Shutdown();
}

TEST_F(ShellTest, BundleErrorsFailCreateWithJsMessage) {
  const auto bundle = std::filesystem::temp_directory_path() / "momo_bad_bundle.js";
  std::ofstream(bundle) << "import { viewports } from 'momo:ui';\nviewports.get(0).render(42);\n";
  Settings settings;
  settings.bundle_path = bundle;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_FALSE(shell.ok());
  EXPECT_NE(shell.status().message().find("must be a Scene"), std::string::npos) << shell.status();
  std::filesystem::remove(bundle);
}

// 宿主在 on_created 里注册的通道处理器先于 JS bundle 就位: bundle 顶层发出的调用不会落空。
TEST_F(ShellTest, HostChannelsRegisteredInOnCreatedSeeTopLevelCalls) {
  Settings settings;
  settings.bundle_path = std::filesystem::path(MOMO_SOURCE_DIR) / "examples/hello_frame/main.js";
  std::vector<std::string> calls;
  auto shell = Shell::Create(runners_, settings, RecordCalls("momo/window", &calls, runners_.platform));
  ASSERT_TRUE(shell.ok()) << shell.status();
  EXPECT_TRUE(PumpUntil([&] { return !calls.empty(); }));
  EXPECT_EQ(calls, (std::vector<std::string>{"setTitle:Momo · hello_frame"}));
  (*shell)->Shutdown();
}

#endif  // MOMO_HAS_JS_RUNTIME


// ---- 多视图 ----

// 宿主: "创建窗口"(一个 RecordingShellDelegate + FakeSurface)并以宿主分配的 id 绑定(视图的创建与关闭
// 都由宿主决定, 引擎只认识视图本身)。
class FakeHost final {
 public:
  struct Window {
    RecordingShellDelegate delegate;
    FakeSurface surface;
  };
  void CreateView(platform::ViewId view_id, Size size) {
    auto window = std::make_unique<Window>();
    shell->AttachView(view_id, &window->delegate, &window->surface);
    shell->OnMetricsChanged(view_id, {{size.width * 2, size.height * 2}, 2.0});
    windows[view_id] = std::move(window);
  }
  void CloseView(platform::ViewId view_id) {
    shell->DetachView(view_id);
    windows.erase(view_id);
  }

  Shell* shell = nullptr;
  std::map<platform::ViewId, std::unique_ptr<Window>> windows;
};

// 一个帧时钟驱动所有视图: 占位驱动每帧为每个视图提交一棵树, 各自画进自己的 surface。
TEST_F(ShellTest, SecondViewGetsItsOwnFramesAndIsRemovedOnDetach) {
  Settings settings;
  settings.refresh_rate = 240;
  auto shell = Shell::Create(runners_, settings);
  ASSERT_TRUE(shell.ok());
  FakeHost host;
  host.shell = shell->get();
  FakeSurface main_surface;
  RecordingShellDelegate main_delegate;
  (*shell)->AttachView(&main_delegate, &main_surface);
  (*shell)->OnMetricsChanged(Metrics(800, 600));
  EXPECT_TRUE(WaitFor([&] { return main_surface.presented.load() >= 1; }));

  // 宿主创建并以 id 1 绑定第二个视图(JS 经宿主的 momo/window create 走的就是这条路径)。
  host.CreateView(1, {320, 200});
  FakeSurface& second = host.windows[1]->surface;
  EXPECT_TRUE(WaitFor([&] { return second.presented.load() >= 1; }));
  EXPECT_EQ(second.last_width.load(), 640);
  EXPECT_EQ(main_surface.last_width.load(), 800);
  EXPECT_EQ((*shell)->attached_views(), (std::vector<platform::ViewId>{0, 1}));

  // 关闭第二个视图: 解绑后其 surface 不再被使用, 主窗口照常出帧。
  host.CloseView(1);
  EXPECT_EQ((*shell)->attached_views(), (std::vector<platform::ViewId>{0}));
  const int main_before = main_surface.presented.load();
  (*shell)->OnMetricsChanged(Metrics(1024, 600));
  EXPECT_TRUE(WaitFor([&] { return main_surface.presented.load() > main_before; }));
  (*shell)->Shutdown();
}

// 视图 0 解绑后帧时钟改用其他视图的 VsyncSource;全部解绑后回退定时器。
TEST_F(ShellTest, VsyncSourceFallsBackToAnotherView) {
  auto shell = Shell::Create(runners_, Settings{});
  ASSERT_TRUE(shell.ok());
  FakeSurface a_surface, b_surface;
  RecordingShellDelegate a, b;
  auto source_a = MakeObject<FakeVsyncSource>();
  auto source_b = MakeObject<FakeVsyncSource>();
  FakeVsyncSource* raw_b = source_b.get();
  (*shell)->AttachView(0, &a, &a_surface, std::move(source_a));
  (*shell)->AttachView(3, &b, &b_surface, std::move(source_b));
  (*shell)->OnMetricsChanged(3, Metrics(100, 100));
  (*shell)->DetachView(0);
  // 视图 3 的来源接管帧时钟: 它的 vsync 能驱动视图 3 出帧。
  EXPECT_TRUE(PumpUntil([&] { return raw_b->requests.load() > 0; }));
  EXPECT_TRUE(WaitFor([&] { return b_surface.presented.load() >= 1; }, std::chrono::milliseconds(3000),
                      platform_loop_.get()));
  (*shell)->Shutdown();
}

}  // namespace
}  // namespace momo::engine
