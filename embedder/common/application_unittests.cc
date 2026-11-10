#include "embedder/common/application.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <fstream>
#include <vector>

#include "base/waitable_event.h"
#include "embedder/common/testing/mock_platform.h"
#include "gtest/gtest.h"

namespace momo::embedder {
namespace {

using State = Application::State;
using Step = Application::ShutdownStep;
using testing::MockPlatformIntegration;
using testing::MockPlatformState;
using testing::MockWindow;

class RecordingObserver final : public Application::Observer {
 public:
  void OnStateChanged(State from, State to) override { states.push_back(to); }
  void OnShutdownStep(Step step) override { steps.push_back(step); }
  std::vector<State> states;
  std::vector<Step> steps;
};

class ApplicationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    resources_ = std::filesystem::temp_directory_path() /
                 ("momo_app_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                  "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(resources_);
    state_ = std::make_shared<MockPlatformState>();
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(resources_, ec);
  }

  void WriteBundle() { std::ofstream(resources_ / "bundle.js") << "// test bundle\n"; }

  std::unique_ptr<Application> MakeApp(AppConfig config = {},
                                       std::function<void(Application::Options&)> tweak = {}) {
    Application::Options options;
    options.config = std::move(config);
    if (tweak) {
      tweak(options);
    }
    options.resources_dir = resources_;
    options.observer = &observer_;
    auto platform = std::make_unique<MockPlatformIntegration>(state_);
    native_ = &platform->native();
    auto app = Application::Create(std::move(options), std::move(platform));
    EXPECT_TRUE(app.ok()) << app.status();
    return std::move(app).value();
  }

  void PostToPlatform(Application& app, Closure task) {
    app.task_runners().platform->PostTask(std::move(task));
  }

  std::filesystem::path resources_;
  std::shared_ptr<MockPlatformState> state_;
  RecordingObserver observer_;
  testing::MockNativeLoop* native_ = nullptr;
};

TEST(ApplicationStateMachine, Transitions) {
  EXPECT_TRUE(Application::IsValidTransition(State::kNotRunning, State::kLaunching));
  EXPECT_TRUE(Application::IsValidTransition(State::kLaunching, State::kRunning));
  EXPECT_TRUE(Application::IsValidTransition(State::kLaunching, State::kTerminating));
  EXPECT_TRUE(Application::IsValidTransition(State::kRunning, State::kSuspended));
  EXPECT_TRUE(Application::IsValidTransition(State::kSuspended, State::kRunning));
  EXPECT_TRUE(Application::IsValidTransition(State::kSuspended, State::kTerminating));
  EXPECT_TRUE(Application::IsValidTransition(State::kRunning, State::kTerminating));

  EXPECT_FALSE(Application::IsValidTransition(State::kNotRunning, State::kRunning));
  EXPECT_FALSE(Application::IsValidTransition(State::kLaunching, State::kSuspended));
  EXPECT_FALSE(Application::IsValidTransition(State::kRunning, State::kLaunching));
  EXPECT_FALSE(Application::IsValidTransition(State::kTerminating, State::kRunning));
  EXPECT_FALSE(Application::IsValidTransition(State::kTerminating, State::kTerminating));
}

TEST_F(ApplicationTest, LaunchRunTerminateInDocumentedOrder) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  EXPECT_EQ(app->state(), State::kRunning);
  ASSERT_NE(state_->window, nullptr);
  EXPECT_TRUE(state_->window->shown());

  // 窗口销毁时: 引擎必须已关闭, 而 ui 线程必须尚未 join(投递的任务仍会被执行)。
  bool engine_gone_at_window_destroy = false;
  std::atomic<bool> ui_alive_at_window_destroy{false};
  Application* raw = app.get();
  state_->on_window_destroyed = [&] {
    engine_gone_at_window_destroy = raw->shell() == nullptr;
    raw->task_runners().ui->PostTask([&] { ui_alive_at_window_destroy = true; });
  };

  PostToPlatform(*app, [raw] { raw->RequestTerminate(); });
  EXPECT_EQ(app->Run(), 0);

  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(observer_.states,
            (std::vector<State>{State::kLaunching, State::kRunning, State::kTerminating}));
  EXPECT_EQ(observer_.steps, (std::vector<Step>{Step::kEngineShutdown, Step::kWindowDestroyed,
                                                Step::kThreadsJoined, Step::kMessageLoopQuit}));
  EXPECT_TRUE(engine_gone_at_window_destroy);
  EXPECT_TRUE(ui_alive_at_window_destroy);
  EXPECT_EQ(state_->windows_destroyed, 1);
  EXPECT_EQ(native_->run_count(), 1);
  EXPECT_EQ(native_->quit_count(), 1);
}

TEST_F(ApplicationTest, WindowCloseIsInterceptedThenTerminatesViaEngine) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());

  bool close_result = true;
  PostToPlatform(*app, [&] { close_result = state_->window->delegate()->OnCloseRequested(); });
  EXPECT_EQ(app->Run(), 0);

  EXPECT_FALSE(close_result) << "native close must be intercepted";
  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(state_->windows_destroyed, 1);
}

// ui 线程卡死(JS 死循环)时, 关闭请求超时后强制退出;正常回复的请求不触发超时。
TEST_F(ApplicationTest, HungUiThreadForcesExitAfterCloseTimeout) {
  WriteBundle();
  std::atomic<int> forced{-1};
  WaitableEvent release_ui;
  auto app = MakeApp({}, [&](Application::Options& o) {
    o.close_timeout = std::chrono::milliseconds(50);
    o.force_exit = [&](int code) {
      forced = code;
      release_ui.Signal();  // 测试中"进程退出"后解除阻塞, 让关闭流程正常收尾
    };
  });
  ASSERT_TRUE(app->Launch().ok());
  app->task_runners().ui->PostTask([&] { release_ui.Wait(); });  // 模拟 JS 卡死
  Application* raw = app.get();
  PostToPlatform(*app, [raw] { raw->RequestTerminate(); });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(forced.load(), 1);
}

TEST_F(ApplicationTest, AnsweredCloseRequestDoesNotTimeOut) {
  WriteBundle();
  bool forced = false;
  auto app = MakeApp({}, [&](Application::Options& o) {
    o.close_timeout = std::chrono::seconds(2);  // 回复远早于此;关闭后延迟任务被丢弃
    o.force_exit = [&](int) { forced = true; };
  });
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  PostToPlatform(*app, [raw] { raw->RequestTerminate(); });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_FALSE(forced);
}

TEST_F(ApplicationTest, RepeatedTerminateRequestsAreCoalesced) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  PostToPlatform(*app, [raw] {
    raw->RequestTerminate();
    raw->RequestTerminate();  // 关窗 + Cmd+Q 同时到达
  });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(observer_.steps.size(), 4u);
  EXPECT_EQ(native_->quit_count(), 1);
}

TEST_F(ApplicationTest, MissingBundleFailsLaunchAndCleansUp) {
  auto app = MakeApp();
  Status status = app->Launch();
  EXPECT_EQ(status.code(), StatusCode::kNotFound) << status;
  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(app->exit_code(), 1);
  EXPECT_EQ(state_->windows_created, 1);
  EXPECT_EQ(state_->windows_destroyed, 1);
  EXPECT_EQ(app->Run(), 1) << "Run must not block after a failed launch";
  EXPECT_EQ(native_->run_count(), 0);
}

TEST_F(ApplicationTest, MissingBundleInDevModeStartsEmptyEngine) {
  AppConfig config;
  config.dev_mode = true;
  auto app = MakeApp(config);
  ASSERT_TRUE(app->Launch().ok());
  EXPECT_EQ(app->state(), State::kRunning);
  ASSERT_NE(app->shell(), nullptr);
  app->Terminate();
  EXPECT_EQ(app->state(), State::kTerminating);
}

TEST_F(ApplicationTest, WindowCreationFailureFailsLaunch) {
  WriteBundle();
  state_->fail_create_window = true;
  auto app = MakeApp();
  Status status = app->Launch();
  EXPECT_EQ(status.code(), StatusCode::kUnavailable);
  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(observer_.steps.size(), 4u);
}

TEST_F(ApplicationTest, MinimizeAndHideSuspendAndResume) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  std::vector<State> seen;
  PostToPlatform(*app, [&, raw] {
    auto* delegate = state_->window->delegate();
    delegate->OnVisibilityChanged(false);  // 最小化
    seen.push_back(raw->state());
    raw->OnAppVisibilityChanged(false);    // 应用隐藏: 仍挂起
    raw->OnAppVisibilityChanged(true);     // 应用显示, 窗口仍最小化: 仍挂起
    seen.push_back(raw->state());
    delegate->OnVisibilityChanged(true);   // 还原
    seen.push_back(raw->state());
    raw->RequestTerminate();
  });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(seen, (std::vector<State>{State::kSuspended, State::kSuspended, State::kRunning}));
  EXPECT_EQ(observer_.states,
            (std::vector<State>{State::kLaunching, State::kRunning, State::kSuspended,
                                State::kRunning, State::kTerminating}));
}

TEST_F(ApplicationTest, DestroyWithoutLaunchIsClean) {
  auto app = MakeApp();
  app.reset();
  EXPECT_EQ(observer_.states, (std::vector<State>{State::kTerminating}));
  EXPECT_EQ(state_->windows_created, 0);
}

TEST_F(ApplicationTest, LaunchTwiceIsRejected) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  EXPECT_EQ(app->Launch().code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(state_->windows_created, 1);
  app->Terminate();
}

TEST_F(ApplicationTest, EngineRequestsReachWindow) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  // 引擎侧(此处从 ui 线程)请求改标题 → platform 线程上的窗口收到。
  std::string title;
  raw->task_runners().ui->PostTask([&, raw] {
    raw->shell()->SetWindowTitle("from engine");
    // platform 队列 FIFO: 这个检查任务排在改标题之后。
    raw->task_runners().platform->PostTask([&, raw] {
      title = state_->window->spec().title;
      raw->Terminate();
    });
  });
  app->Run();
  EXPECT_EQ(title, "from engine");
}

// 多窗口: 引擎请求创建视图 → Application 建窗口并绑定;关闭按钮询问 JS 后只关该窗口;
// 退出应用时其他窗口一并销毁。
TEST_F(ApplicationTest, EngineCreatedWindowsOpenCloseAndShutDown) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  size_t open_after_create = 0;
  std::string title;
  size_t open_after_close = 0;
  std::vector<engine::platform::ViewId> attached;
  // ui 线程上发起(JS createView 走的就是这条路): Shell 投递到 platform 线程分配 id 并让宿主建窗口。
  raw->task_runners().ui->PostTask([&, raw] {
    raw->shell()->CreateView(/*request=*/42, {"second", {400, 300}, {100, 100}});
    raw->task_runners().platform->PostTask([&, raw] {
      open_after_create = raw->extra_window_count();
      attached = raw->shell()->attached_views();
      ASSERT_EQ(state_->windows.size(), 2u);
      MockWindow* second = state_->windows[1];
      title = second->spec().title;
      // 用户点第二个窗口的关闭按钮(JS 未拦截): 只关闭它。
      EXPECT_FALSE(second->delegate()->OnCloseRequested()) << "always intercepted";
    });
  });
  // 关闭需要往返 ui 线程: 稍后检查, 然后再开一个窗口并直接退出应用。
  raw->task_runners().platform->PostDelayedTask(
      [&, raw] {
        open_after_close = raw->extra_window_count();
        raw->task_runners().ui->PostTask([raw] {
          raw->shell()->CreateView(43, {"third", {200, 200}, {100, 100}});
          raw->task_runners().platform->PostTask([raw] { raw->RequestTerminate(); });
        });
      },
      std::chrono::milliseconds(200));
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(open_after_create, 1u);
  EXPECT_EQ(attached, (std::vector<engine::platform::ViewId>{0, 1}));
  EXPECT_EQ(title, "second");
  EXPECT_EQ(open_after_close, 0u);
  EXPECT_EQ(state_->windows_created, 3);
  EXPECT_EQ(state_->windows_destroyed, 3) << "the third window is destroyed on shutdown";
}

}  // namespace
}  // namespace momo::embedder
