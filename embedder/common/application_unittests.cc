#include "embedder/common/application.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <fstream>
#include <vector>

#include "base/waitable_event.h"
#include "engine/common/standard_codec.h"
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

  // 导入 momo:services: JS 端点就位(宿主发往 JS 的询问由 JS 侧回复;没有处理器的通道回复空 = 允许)。
  void WriteBundle() {
    std::ofstream(resources_ / "bundle.js") << "import 'momo:services';\n// test bundle\n";
  }

  // 模拟 JS 经宿主通道发出的方法调用(从 ui 线程发送, 与 momo:services.send 相同的路径);
  // 结果在 ui 线程回调。
  static void SendFromJs(Application* app, const std::string& channel, engine::MethodCall call,
                         std::function<void(StatusOr<engine::EncodableValue>)> done) {
    app->task_runners().ui->PostTask([app, channel, call = std::move(call), done = std::move(done)]() mutable {
      app->shell()->hub()->SendToNative(
          channel, engine::EncodeMethodCall(call), app->task_runners().ui,
          [done = std::move(done)](std::optional<engine::MessageBytes> reply) {
            done(reply.has_value() ? engine::DecodeEnvelope(*reply)
                                   : StatusOr<engine::EncodableValue>(NotFoundError("no handler")));
          });
    });
  }

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

  // 无 bundle 的 --dev 引擎没有 JS 运行时: 测试自己充当 JS 端点(platform 线程), 记录宿主发往 JS 的消息
  // (channel:method), 询问按 answer 回复(缺省允许)。
  struct FakeJs {
    std::vector<std::string> messages;
    std::function<bool(const std::string& what)> answer;
    // 只取询问(需要回复的消息)。
    std::vector<std::string> questions;
  };
  std::unique_ptr<Application> LaunchWithFakeJs(FakeJs& js, AppConfig config = {}) {
    config.dev_mode = true;
    config.inspector_port = -1;
    auto app = MakeApp(std::move(config), [](Application::Options& o) { o.hot_restart_poll = {}; });
    EXPECT_TRUE(app->Launch().ok());
    app->shell()->hub()->SetJsEndpoint(
        app->task_runners().platform,
        [&js](const std::string& channel, engine::MessageBytes message, engine::MessageReply reply) {
          StatusOr<engine::MethodCall> call = engine::DecodeMethodCall(message);
          const std::string what = channel + ":" + (call.ok() ? call->method : "<bad>");
          js.messages.push_back(what);
          if (reply) {
            js.questions.push_back(what);
            const bool allow = js.answer ? js.answer(what) : true;
            reply.Send(engine::EncodeSuccessEnvelope(engine::EncodableValue(allow)));
          }
        });
    return app;
  }
  static bool Contains(const std::vector<std::string>& v, const std::string& item) {
    return std::find(v.begin(), v.end(), item) != v.end();
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

  PostToPlatform(*app, [raw] { raw->Terminate(); });
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

// 关闭按钮总是先被拦截: 询问 JS(momo/window closeRequested)后关闭该窗口。最后一个窗口关闭不退出应用:
// 只通知 JS(momo/app windowAllClosed), 引擎与 JS 继续运行(零窗口是合法状态)。
TEST_F(ApplicationTest, ClosingTheLastWindowKeepsTheAppRunning) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  bool close_result = true;
  State after = State::kNotRunning;
  size_t windows_after = 1;
  PostToPlatform(*app, [&] { close_result = state_->window->delegate()->OnCloseRequested(); });
  raw->task_runners().platform->PostDelayedTask(
      [&, raw] {
        after = raw->state();
        windows_after = raw->windows().count();
        raw->Terminate();
      },
      std::chrono::milliseconds(100));
  EXPECT_EQ(app->Run(), 0);
  EXPECT_FALSE(close_result) << "native close must be intercepted";
  EXPECT_EQ(after, State::kRunning);
  EXPECT_EQ(windows_after, 0u);
  EXPECT_TRUE(Contains(js.questions, "momo/window:closeRequested"));
  EXPECT_TRUE(Contains(js.messages, "momo/app:windowAllClosed"));
}

// 退出流程: beforeQuit → 逐个询问窗口 → willQuit → 有序关闭。退出期间不发 windowAllClosed。
TEST_F(ApplicationTest, QuitAsksBeforeQuitThenWindowsThenWillQuit) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  PostToPlatform(*app, [raw] { raw->Quit(); });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(js.questions, (std::vector<std::string>{"momo/app:beforeQuit", "momo/window:closeRequested",
                                                    "momo/app:willQuit"}));
  EXPECT_FALSE(Contains(js.messages, "momo/app:windowAllClosed"));
  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(state_->windows_destroyed, 1);
}

TEST_F(ApplicationTest, QuitIsCancelledByBeforeQuit) {
  FakeJs js;
  js.answer = [](const std::string& what) { return what != "momo/app:beforeQuit"; };
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  bool quitting_after = true;
  PostToPlatform(*app, [raw] { raw->Quit(); });
  raw->task_runners().platform->PostDelayedTask(
      [&, raw] {
        quitting_after = raw->is_quitting();
        raw->Terminate();
      },
      std::chrono::milliseconds(50));
  app->Run();
  EXPECT_EQ(js.questions, (std::vector<std::string>{"momo/app:beforeQuit"}));
  EXPECT_FALSE(quitting_after);
  EXPECT_EQ(state_->windows_destroyed, 1) << "destroyed only by the final Terminate";
}

// 任一窗口拒绝关闭: 退出中止, 已允许的窗口照常关闭(同 Electron)。
TEST_F(ApplicationTest, QuitAbortsWhenAWindowRefuses) {
  FakeJs js;
  js.answer = [](const std::string& what) { return what != "momo/window:closeRequested"; };
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  State after = State::kNotRunning;
  size_t windows_after = 0;
  PostToPlatform(*app, [raw] { raw->Quit(); });
  raw->task_runners().platform->PostDelayedTask(
      [&, raw] {
        after = raw->state();
        windows_after = raw->windows().count();
        raw->Terminate();
      },
      std::chrono::milliseconds(50));
  app->Run();
  EXPECT_EQ(after, State::kRunning);
  EXPECT_EQ(windows_after, 1u);
  EXPECT_FALSE(Contains(js.questions, "momo/app:willQuit"));
}

// willQuit 取消: 窗口已全部关闭, 应用无窗口继续运行。
TEST_F(ApplicationTest, WillQuitCancelKeepsAppRunningWithoutWindows) {
  FakeJs js;
  js.answer = [](const std::string& what) { return what != "momo/app:willQuit"; };
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  State after = State::kNotRunning;
  size_t windows_after = 1;
  PostToPlatform(*app, [raw] { raw->Quit(); });
  raw->task_runners().platform->PostDelayedTask(
      [&, raw] {
        after = raw->state();
        windows_after = raw->windows().count();
        raw->Terminate();
      },
      std::chrono::milliseconds(50));
  app->Run();
  EXPECT_EQ(after, State::kRunning);
  EXPECT_EQ(windows_after, 0u);
}

// Exit: 不询问 JS, 仍按文档顺序有序关闭, 返回给定退出码。
TEST_F(ApplicationTest, ExitSkipsQuestionsButShutsDownInOrder) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  PostToPlatform(*app, [raw] { raw->Exit(7); });
  EXPECT_EQ(app->Run(), 7);
  EXPECT_TRUE(js.questions.empty());
  EXPECT_EQ(observer_.steps, (std::vector<Step>{Step::kEngineShutdown, Step::kWindowDestroyed,
                                                Step::kThreadsJoined, Step::kMessageLoopQuit}));
}

// JS 经 momo/app 调用 exit / quit。
TEST_F(ApplicationTest, JsExitReachesTheApplication) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  SendFromJs(raw, "momo/app", {"exit", engine::EncodableMap{{"code", int64_t{4}}}},
             [](StatusOr<engine::EncodableValue> result) { EXPECT_TRUE(result.ok()) << result.status(); });
  EXPECT_EQ(app->Run(), 4);
}

TEST_F(ApplicationTest, JsQuitReachesTheApplication) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  SendFromJs(app.get(), "momo/app", {"quit", engine::EncodableValue()},
             [](StatusOr<engine::EncodableValue> result) { EXPECT_TRUE(result.ok()) << result.status(); });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(app->state(), State::kTerminating);
}

// 应用级平台能力(momo/app hide / focus / getState)经 PlatformApp。
TEST_F(ApplicationTest, AppMethodsReachThePlatformApp) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  auto& mock_app = static_cast<testing::MockApp&>(raw->platform().app());
  std::string state;
  SendFromJs(raw, "momo/app", {"hide", engine::EncodableValue()}, [&, raw](StatusOr<engine::EncodableValue> r) {
    EXPECT_TRUE(r.ok()) << r.status();
    SendFromJs(raw, "momo/app", {"focus", engine::EncodableMap{{"steal", true}}},
               [&, raw](StatusOr<engine::EncodableValue> r) {
                 EXPECT_TRUE(r.ok()) << r.status();
                 SendFromJs(raw, "momo/app", {"getState", engine::EncodableValue()},
                            [&, raw](StatusOr<engine::EncodableValue> r) {
                              ASSERT_TRUE(r.ok()) << r.status();
                              state = r->Find("state")->AsString().value_or("");
                              raw->task_runners().platform->PostTask([raw] { raw->Terminate(); });
                            });
               });
  });
  app->Run();
  EXPECT_TRUE(mock_app.hidden);
  EXPECT_EQ(mock_app.activations, 1);
  EXPECT_TRUE(mock_app.last_steal);
  EXPECT_EQ(state, "running");
}

// 用户点击 Dock 图标(applicationShouldHandleReopen)→ momo/app activate;前台状态 → activeChanged。
TEST_F(ApplicationTest, ReopenAndActivationReachJs) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  PostToPlatform(*app, [raw] {
    raw->OnAppActiveChanged(true);
    raw->OnReopen(false);
    raw->Terminate();
  });
  app->Run();
  EXPECT_TRUE(Contains(js.messages, "momo/app:activeChanged"));
  EXPECT_TRUE(Contains(js.messages, "momo/app:activate"));
}

// 不建启动窗口: 引擎没有隐式视图, 窗口全部由 JS 打开。
TEST_F(ApplicationTest, NoStartupWindowLaunches) {
  WriteBundle();
  AppConfig config;
  config.startup_window = false;
  auto app = MakeApp(config);
  ASSERT_TRUE(app->Launch().ok());
  EXPECT_EQ(app->state(), State::kRunning);
  EXPECT_EQ(state_->windows_created, 0);
  EXPECT_TRUE(app->shell()->attached_views().empty());
  app->Terminate();
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
  PostToPlatform(*app, [raw] { raw->Quit(); });
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
  PostToPlatform(*app, [raw] { raw->Quit(); });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_FALSE(forced);
}

TEST_F(ApplicationTest, RepeatedTerminateRequestsAreCoalesced) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  PostToPlatform(*app, [raw] {
    raw->Quit();
    raw->Quit();  // 关窗 + Cmd+Q 同时到达
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

// Suspended 只看应用是否被隐藏;窗口最小化只影响该窗口的视图出帧, 不改变应用状态。
TEST_F(ApplicationTest, MinimizingWindowsDoesNotSuspendButHidingTheAppDoes) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  std::vector<State> seen;
  PostToPlatform(*app, [&, raw] {
    ASSERT_TRUE(state_->window->Minimize().ok());
    seen.push_back(raw->state());
    raw->OnAppVisibilityChanged(false);  // 应用隐藏: 挂起
    seen.push_back(raw->state());
    raw->OnAppVisibilityChanged(true);
    seen.push_back(raw->state());
    raw->Quit();
  });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(seen, (std::vector<State>{State::kRunning, State::kSuspended, State::kRunning}));
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

// 窗口请求不经引擎: JS 经宿主注册的 momo/window 通道直接到达窗口(platform 线程)。
TEST_F(ApplicationTest, WindowChannelRequestsReachTheWindow) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  std::string title;
  std::string unknown_view_error;
  SendFromJs(raw, "momo/window",
             {"setTitle", engine::EncodableMap{{"view", int64_t{0}}, {"title", "from js"}}},
             [&, raw](StatusOr<engine::EncodableValue> result) {
               EXPECT_TRUE(result.ok()) << result.status();
               raw->task_runners().platform->PostTask([&, raw] {
                 title = state_->window->spec().title;
                 SendFromJs(raw, "momo/window",
                            {"setTitle", engine::EncodableMap{{"view", int64_t{9}}, {"title", "x"}}},
                            [&, raw](StatusOr<engine::EncodableValue> result) {
                              unknown_view_error = std::string(result.status().message());
                              raw->task_runners().platform->PostTask([raw] { raw->Terminate(); });
                            });
               });
             });
  app->Run();
  EXPECT_EQ(title, "from js");
  EXPECT_NE(unknown_view_error.find("no window for view 9"), std::string::npos) << unknown_view_error;
}

// 多窗口: JS 经 momo/window create 打开窗口 → 宿主分配 id、建窗口并绑定引擎;关闭按钮询问 JS 后只关该窗口;
// 退出应用时其他窗口一并销毁。
TEST_F(ApplicationTest, JsCreatedWindowsOpenCloseAndShutDown) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  int64_t created_id = -1;
  size_t open_after_create = 0;
  std::vector<engine::platform::ViewId> attached;
  std::string title;
  size_t open_after_close = 0;
  SendFromJs(raw, "momo/window",
             {"create", engine::EncodableMap{{"title", "second"}, {"width", 400.0}, {"height", 300.0}}},
             [&, raw](StatusOr<engine::EncodableValue> result) {
               ASSERT_TRUE(result.ok()) << result.status();
               created_id = result->AsInt().value_or(-1);
               raw->task_runners().platform->PostTask([&, raw] {
                 open_after_create = raw->windows().count();
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
        open_after_close = raw->windows().count();
        SendFromJs(raw, "momo/window",
                   {"create", engine::EncodableMap{{"title", "third"}, {"width", 200.0}, {"height", 200.0}}},
                   [raw](StatusOr<engine::EncodableValue>) {
                     raw->task_runners().platform->PostTask([raw] { raw->Quit(); });
                   });
      },
      std::chrono::milliseconds(200));
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(created_id, 1);
  EXPECT_EQ(open_after_create, 2u);
  EXPECT_EQ(attached, (std::vector<engine::platform::ViewId>{0, 1}));
  EXPECT_EQ(title, "second");
  EXPECT_EQ(open_after_close, 1u);
  EXPECT_EQ(state_->windows_created, 3);
  EXPECT_EQ(state_->windows_destroyed, 3) << "the third window is destroyed on shutdown";
}

// 所有窗口对等: 启动窗口(隐式视图 0)关闭后应用照常运行, 由其他窗口继续;最后一个关闭也不退出。
TEST_F(ApplicationTest, StartupWindowCanCloseWhileOthersStayOpen) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  State after_first_close = State::kNotRunning;
  State after_last_close = State::kNotRunning;
  std::vector<engine::platform::ViewId> attached;
  SendFromJs(raw, "momo/window",
             {"create", engine::EncodableMap{{"width", 300.0}, {"height", 200.0}}},
             [&, raw](StatusOr<engine::EncodableValue> result) {
               ASSERT_TRUE(result.ok()) << result.status();
               SendFromJs(raw, "momo/window", {"close", engine::EncodableMap{{"view", int64_t{0}}}},
                          [&, raw](StatusOr<engine::EncodableValue>) {
                            raw->task_runners().platform->PostDelayedTask(
                                [&, raw] {
                                  after_first_close = raw->state();
                                  attached = raw->shell()->attached_views();
                                  SendFromJs(raw, "momo/window",
                                             {"close", engine::EncodableMap{{"view", int64_t{1}}}},
                                             [](StatusOr<engine::EncodableValue>) {});
                                },
                                std::chrono::milliseconds(50));
                          });
             });
  raw->task_runners().platform->PostDelayedTask(
      [&, raw] {
        after_last_close = raw->state();
        raw->Terminate();
      },
      std::chrono::milliseconds(300));
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(after_first_close, State::kRunning);
  EXPECT_EQ(attached, (std::vector<engine::platform::ViewId>{1}));
  EXPECT_EQ(after_last_close, State::kRunning) << "closing the last window does not exit";
  EXPECT_EQ(state_->windows_destroyed, 2);
}

// 输入法会话交给获得焦点的窗口(momo/textinput 由宿主注册, 不经引擎)。
TEST_F(ApplicationTest, TextInputGoesToTheFocusedWindow) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  Application* raw = app.get();
  bool first_shown = false, second_shown = false;
  SendFromJs(raw, "momo/window", {"create", engine::EncodableMap{{"width", 300.0}, {"height", 200.0}}},
             [&, raw](StatusOr<engine::EncodableValue>) {
               raw->task_runners().platform->PostTask([&, raw] {
                 state_->windows[1]->delegate()->OnFocusChanged(true);
                 SendFromJs(raw, "momo/textinput", {"show", engine::EncodableValue()},
                            [&, raw](StatusOr<engine::EncodableValue>) {
                              raw->task_runners().platform->PostTask([&, raw] {
                                first_shown = state_->windows[0]->text_input().shown;
                                second_shown = state_->windows[1]->text_input().shown;
                                raw->Terminate();
                              });
                            });
               });
             });
  app->Run();
  EXPECT_FALSE(first_shown);
  EXPECT_TRUE(second_shown);
}

}  // namespace
}  // namespace momo::embedder
