#include "embedder/common/application.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <thread>
#include <vector>

#include "base/event_loop.h"
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

  // 打包后的应用: <resources>/app/{package.json, main.js}。
  void WriteApp(const char* source) {
    std::filesystem::create_directories(resources_ / "app");
    std::ofstream(resources_ / "app" / "package.json") << R"({"main": "main.js"})";
    std::ofstream(resources_ / "app" / "main.js") << source;
  }
  // 导入 momo:services: JS 端点就位(宿主发往 JS 的询问由 JS 侧回复;没有处理器的通道回复空 = 允许)。
  void WriteBundle() { WriteApp("import 'momo:services';\n// test bundle\n"); }

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

  // Launch 并泵 platform 循环, 直到 JS 运行时的启动结果到达(Launching 结束)。成功时应用处于 Running。
  static Status LaunchAndWait(Application& app) {
    MOMO_RETURN_IF_ERROR(app.Launch());
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (app.state() == State::kLaunching && Clock::now() < deadline) {
      EventLoop::Current()->RunUntilIdle();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (app.state() == State::kLaunching) {
      return UnavailableError("the JS runtime did not report a result");
    }
    return app.state() == State::kTerminating ? FailedPreconditionError("launch failed") : Status::Ok();
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
    // 这个 bundle 不导入 momo:services: 真实的 JS 端点不会安装, 由下面的假 JS 充当 JS 侧。
    WriteApp("// test bundle: the test plays the JS side\n");
    auto app = MakeApp(std::move(config));
    EXPECT_TRUE(LaunchAndWait(*app).ok());
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

// Launch 不等待 bundle 执行: 返回时仍是 Launching(窗口已显示, 引擎已创建, 通道与窗口已绑定);
// 进入事件循环后 JS 运行时的启动结果到达, 才进入 Running。
TEST_F(ApplicationTest, LaunchReturnsBeforeTheBundleResultArrives) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  EXPECT_EQ(app->state(), State::kLaunching);
  ASSERT_NE(state_->window, nullptr);
  EXPECT_TRUE(state_->window->shown());
  ASSERT_NE(app->shell(), nullptr);
  EXPECT_EQ(app->shell()->attached_views(), (std::vector<engine::platform::ViewId>{0}));
  Application* raw = app.get();
  std::vector<State> seen;
  // 循环里轮询: 启动结果到达后记录状态并退出。
  std::function<void()> poll = [&, raw] {
    if (raw->state() == State::kLaunching) {
      raw->task_runners().platform->PostDelayedTask(poll, std::chrono::milliseconds(1));
      return;
    }
    seen.push_back(raw->state());
    raw->Exit(0);
  };
  PostToPlatform(*app, poll);
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(seen, (std::vector<State>{State::kRunning}));
  EXPECT_EQ(observer_.states,
            (std::vector<State>{State::kLaunching, State::kRunning, State::kTerminating}));
}

// 启动期间(bundle 还没执行完)请求退出: 走同一条退出流程(询问排在 bundle 之后, 没有处理器即允许), 不是直接终止
// ——请求返回时什么都还没销毁。
TEST_F(ApplicationTest, QuitWhileLaunchingFollowsTheQuitFlow) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok());
  app->Quit();
  EXPECT_NE(app->state(), State::kTerminating) << "Quit only starts the flow";
  EXPECT_TRUE(app->is_quitting());
  EXPECT_NE(app->shell(), nullptr);
  EXPECT_EQ(state_->windows_destroyed, 0);
  EXPECT_EQ(app->Run(), 0);
  ASSERT_FALSE(observer_.states.empty());
  EXPECT_EQ(observer_.states.front(), State::kLaunching);
  EXPECT_EQ(observer_.states.back(), State::kTerminating);
  EXPECT_EQ(state_->windows_destroyed, 1);
}

// 退出的顺序(同 Electron): 窗口先关闭(JS 还在) → 消息循环结束 → 循环之后逐级销毁, JS 环境与引擎最先,
// 线程最后 join。退出请求本身不销毁引擎。
TEST_F(ApplicationTest, ExitClosesWindowsThenTearsDownAfterTheLoop) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  EXPECT_EQ(app->state(), State::kRunning);
  ASSERT_NE(state_->window, nullptr);
  EXPECT_TRUE(state_->window->shown());

  bool engine_alive_at_window_destroy = false;
  bool engine_alive_after_exit = false;
  Application* raw = app.get();
  state_->on_window_destroyed = [&] { engine_alive_at_window_destroy = raw->shell() != nullptr; };

  PostToPlatform(*app, [&, raw] {
    raw->Exit(0);
    engine_alive_after_exit = raw->shell() != nullptr && !raw->shell()->is_shut_down();
  });
  EXPECT_EQ(app->Run(), 0);

  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(observer_.states,
            (std::vector<State>{State::kLaunching, State::kRunning, State::kTerminating}));
  EXPECT_EQ(observer_.steps, (std::vector<Step>{Step::kMessageLoopQuit, Step::kEngineShutdown,
                                                Step::kWindowDestroyed, Step::kThreadsJoined}));
  EXPECT_TRUE(engine_alive_at_window_destroy) << "windows close while JS is still running";
  EXPECT_TRUE(engine_alive_after_exit) << "Exit only requests the loop to end; nothing is torn down in it";
  EXPECT_EQ(app->shell(), nullptr) << "torn down by the time Run returns";
  EXPECT_EQ(state_->windows_destroyed, 1);
  EXPECT_EQ(native_->run_count(), 1);
  EXPECT_EQ(native_->quit_count(), 1);
}

// 没有经过退出流程就销毁(还开着窗口): 销毁顺序仍是 JS 环境与引擎 → 窗口 → 线程。
TEST_F(ApplicationTest, DestroyingARunningApplicationTearsDownEngineBeforeWindows) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  // 窗口销毁时: 引擎必须已关闭, 而 ui 线程必须尚未 join(投递的任务仍会被执行)。
  bool engine_gone_at_window_destroy = false;
  std::atomic<bool> ui_alive_at_window_destroy{false};
  Application* raw = app.get();
  state_->on_window_destroyed = [&] {
    engine_gone_at_window_destroy = raw->shell()->is_shut_down();
    raw->task_runners().ui->PostTask([&] { ui_alive_at_window_destroy = true; });
  };
  app.reset();
  EXPECT_EQ(observer_.steps, (std::vector<Step>{Step::kMessageLoopQuit, Step::kEngineShutdown,
                                                Step::kWindowDestroyed, Step::kThreadsJoined}));
  EXPECT_TRUE(engine_gone_at_window_destroy);
  EXPECT_TRUE(ui_alive_at_window_destroy);
  EXPECT_EQ(state_->windows_destroyed, 1);
}

// 关闭按钮总是先被拦截: 询问 JS(momo/window closeRequested)后关闭该窗口。最后一个窗口关闭不退出应用:
// 只通知 JS(momo/app windowAllClosed), 引擎与 JS 继续运行(零窗口是合法状态)。
TEST_F(ApplicationTest, ClosingTheLastWindowKeepsTheAppRunning) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  State after = State::kNotRunning;
  size_t windows_after = 1;
  PostToPlatform(*app, [&] { state_->window->delegate()->OnCloseRequested(); });
  raw->task_runners().platform->PostDelayedTask(
      [&, raw] {
        after = raw->state();
        windows_after = raw->windows().count();
        raw->Exit(0);
      },
      std::chrono::milliseconds(100));
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(after, State::kRunning);
  EXPECT_EQ(windows_after, 0u);
  EXPECT_TRUE(Contains(js.questions, "momo/window:closeRequested"));
  EXPECT_TRUE(Contains(js.messages, "momo/app:windowAllClosed"));
}

// 退出流程: beforeQuit → 逐个询问窗口 → willQuit → 进入 terminating(JS 收得到: 此时它还活着)→ 循环结束后销毁。
// 退出期间不发 windowAllClosed。
TEST_F(ApplicationTest, QuitAsksBeforeQuitThenWindowsThenWillQuit) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  PostToPlatform(*app, [raw] { raw->Quit(); });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(js.questions, (std::vector<std::string>{"momo/app:beforeQuit", "momo/window:closeRequested",
                                                    "momo/app:willQuit"}));
  ASSERT_FALSE(js.messages.empty());
  EXPECT_EQ(js.messages.back(), "momo/app:stateChanged") << "JS is told about 'terminating' before teardown";
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
        raw->Exit(0);
      },
      std::chrono::milliseconds(50));
  app->Run();
  EXPECT_EQ(js.questions, (std::vector<std::string>{"momo/app:beforeQuit"}));
  EXPECT_FALSE(quitting_after);
  EXPECT_EQ(state_->windows_destroyed, 1) << "destroyed only by the final Exit";
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
        raw->Exit(0);
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
        raw->Exit(0);
      },
      std::chrono::milliseconds(50));
  app->Run();
  EXPECT_EQ(after, State::kRunning);
  EXPECT_EQ(windows_after, 0u);
}

// Exit: 不询问 JS, 窗口照常关闭(JS 收到 closed), 按文档顺序销毁, 返回给定退出码。
TEST_F(ApplicationTest, ExitSkipsQuestionsButShutsDownInOrder) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  PostToPlatform(*app, [raw] { raw->Exit(7); });
  EXPECT_EQ(app->Run(), 7);
  EXPECT_TRUE(js.questions.empty());
  EXPECT_TRUE(Contains(js.messages, "momo/window:closed"));
  EXPECT_EQ(observer_.steps, (std::vector<Step>{Step::kMessageLoopQuit, Step::kEngineShutdown,
                                                Step::kWindowDestroyed, Step::kThreadsJoined}));
}

// JS 经 momo/app 调用 exit / quit。
TEST_F(ApplicationTest, JsExitReachesTheApplication) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  Application* raw = app.get();
  SendFromJs(raw, "momo/app", {"exit", engine::EncodableMap{{"code", int64_t{4}}}},
             [](StatusOr<engine::EncodableValue> result) { EXPECT_TRUE(result.ok()) << result.status(); });
  EXPECT_EQ(app->Run(), 4);
}

TEST_F(ApplicationTest, JsQuitReachesTheApplication) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  SendFromJs(app.get(), "momo/app", {"quit", engine::EncodableValue()},
             [](StatusOr<engine::EncodableValue> result) { EXPECT_TRUE(result.ok()) << result.status(); });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(app->state(), State::kTerminating);
}

// 应用级平台能力(momo/app hide / focus)经 PlatformApp。
TEST_F(ApplicationTest, AppMethodsReachThePlatformApp) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  Application* raw = app.get();
  auto& mock_app = static_cast<testing::MockApp&>(raw->platform().app());
  SendFromJs(raw, "momo/app", {"hide", engine::EncodableValue()}, [&, raw](StatusOr<engine::EncodableValue> r) {
    EXPECT_TRUE(r.ok()) << r.status();
    SendFromJs(raw, "momo/app", {"focus", engine::EncodableMap{{"steal", true}}},
               [&, raw](StatusOr<engine::EncodableValue> r) {
                 EXPECT_TRUE(r.ok()) << r.status();
                 raw->task_runners().platform->PostTask([raw] { raw->Exit(0); });
               });
  });
  app->Run();
  EXPECT_TRUE(mock_app.hidden);
  EXPECT_EQ(mock_app.activations, 1);
  EXPECT_TRUE(mock_app.last_steal);
}

// 用户点击 Dock 图标(applicationShouldHandleReopen)→ momo/app activate;前台状态 → activeChanged。
TEST_F(ApplicationTest, ReopenAndActivationReachJs) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  PostToPlatform(*app, [raw] {
    raw->OnAppActiveChanged(true);
    raw->OnReopen(false);
    raw->task_runners().platform->PostTask([raw] { raw->Exit(0); });
  });
  app->Run();
  EXPECT_TRUE(Contains(js.messages, "momo/app:activeChanged"));
  EXPECT_TRUE(Contains(js.messages, "momo/app:activate"));
}

// 不建启动窗口: 引擎里没有视图, 窗口全部由 JS 打开。
TEST_F(ApplicationTest, NoStartupWindowLaunches) {
  WriteBundle();
  AppConfig config;
  config.startup_window = false;
  auto app = MakeApp(config);
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  EXPECT_EQ(app->state(), State::kRunning);
  EXPECT_EQ(state_->windows_created, 0);
  EXPECT_TRUE(app->shell()->attached_views().empty());
  app->Exit(0);
}

TEST_F(ApplicationTest, RepeatedQuitRequestsAreCoalesced) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  Application* raw = app.get();
  PostToPlatform(*app, [raw] {
    raw->Quit();
    raw->Quit();  // 关窗 + Cmd+Q 同时到达
  });
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(observer_.steps.size(), 4u);
  EXPECT_EQ(native_->quit_count(), 1);
}

#if MOMO_HAS_JS_RUNTIME
// 入口不存在不会提前检查: JS 运行时加载时才发现, 结果在事件循环里到达。启动失败, 有序清理, 退出码非零——
// 无论是包内的应用还是命令行给出的, 也无论是否 --dev。
TEST_F(ApplicationTest, MissingBundleFailsLaunchAndCleansUp) {
  auto app = MakeApp();
  ASSERT_TRUE(app->Launch().ok()) << "the bundle is not checked synchronously";
  EXPECT_EQ(state_->windows_created, 1);
  EXPECT_EQ(app->Run(), 1) << "Run returns once the failed launch has terminated the app";
  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(app->exit_code(), 1);
  EXPECT_EQ(state_->windows_destroyed, 1);
  EXPECT_EQ(observer_.states, (std::vector<State>{State::kLaunching, State::kTerminating}));
  EXPECT_EQ(observer_.steps.size(), 4u);
}

TEST_F(ApplicationTest, MissingExplicitBundleFailsLaunchEvenInDevMode) {
  WriteBundle();  // 包内的应用存在, 但命令行指定了另一个
  AppConfig config;
  config.dev_mode = true;
  config.inspector_port = -1;
  config.app_path = resources_ / "nope.js";
  auto app = MakeApp(config);
  ASSERT_TRUE(app->Launch().ok());
  EXPECT_EQ(app->Run(), 1);
  EXPECT_EQ(app->state(), State::kTerminating);
  EXPECT_EQ(state_->windows_destroyed, 1);
}
#endif  // MOMO_HAS_JS_RUNTIME

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
  ASSERT_TRUE(LaunchAndWait(*app).ok());
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
  app->Exit(0);
}

// 窗口请求不经引擎: JS 经宿主注册的 momo/window 通道直接到达窗口(platform 线程)。
TEST_F(ApplicationTest, WindowChannelRequestsReachTheWindow) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
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
                              raw->task_runners().platform->PostTask([raw] { raw->Exit(0); });
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
  ASSERT_TRUE(LaunchAndWait(*app).ok());
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
               const engine::EncodableValue* view = result->Find("view");
               created_id = view != nullptr ? view->AsInt().value_or(-1) : -1;
               raw->task_runners().platform->PostTask([&, raw] {
                 open_after_create = raw->windows().count();
                 attached = raw->shell()->attached_views();
                 ASSERT_EQ(state_->windows.size(), 2u);
                 MockWindow* second = state_->windows[1];
                 title = second->spec().title;
                 // 用户点第二个窗口的关闭按钮(JS 未拦截): 只关闭它。
                 second->delegate()->OnCloseRequested();
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
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  Application* raw = app.get();
  State after_first_close = State::kNotRunning;
  State after_last_close = State::kNotRunning;
  std::vector<engine::platform::ViewId> attached;
  // 帧时钟(应用只有一个来源)跟随 id 最小的窗口: 启动窗口 → 它关闭后剩下的那个 → 没有窗口(主显示器)。
  EXPECT_EQ(state_->vsync_window, state_->window);
  bool vsync_follows_remaining = false;
  bool vsync_follows_none = false;
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
                                  vsync_follows_remaining =
                                      state_->windows.size() == 1 && state_->vsync_window == state_->windows[0];
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
        vsync_follows_none = state_->vsync_window == nullptr;
        raw->Exit(0);
      },
      std::chrono::milliseconds(300));
  EXPECT_EQ(app->Run(), 0);
  EXPECT_EQ(after_first_close, State::kRunning);
  EXPECT_EQ(attached, (std::vector<engine::platform::ViewId>{1}));
  EXPECT_TRUE(vsync_follows_remaining);
  EXPECT_TRUE(vsync_follows_none);
  EXPECT_EQ(state_->vsync_sources_created, 1) << "one vsync source per application, not per window";
  EXPECT_EQ(after_last_close, State::kRunning) << "closing the last window does not exit";
  EXPECT_EQ(state_->windows_destroyed, 2);
}

// 输入法会话交给获得焦点的窗口(momo/textinput 由宿主注册, 不经引擎)。
TEST_F(ApplicationTest, TextInputGoesToTheFocusedWindow) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
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
                                raw->Exit(0);
                              });
                            });
               });
             });
  app->Run();
  EXPECT_FALSE(first_shown);
  EXPECT_TRUE(second_shown);
}

// 平台没有系统标准动作的 role(Windows)由 Application 执行: 窗口类作用于获得焦点的窗口, 编辑类经
// momo/textinput 的编辑命令交给 JS, quit 走退出流程。
TEST_F(ApplicationTest, MenuRolesActOnTheFocusedWindowTheTextFieldAndTheApp) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  MenuDelegate* menus = state_->menus->delegate;
  ASSERT_NE(menus, nullptr);
  MockWindow* window = state_->window;

  menus->OnMenuRoleActivated(MenuRole::kMinimize);  // 没有获得焦点的窗口: 什么也不做
  EXPECT_FALSE(window->state().minimized);
  window->delegate()->OnFocusChanged(true);
  menus->OnMenuRoleActivated(MenuRole::kZoom);
  EXPECT_TRUE(window->state().maximized);
  menus->OnMenuRoleActivated(MenuRole::kZoom);
  EXPECT_FALSE(window->state().maximized);
  menus->OnMenuRoleActivated(MenuRole::kMinimize);
  EXPECT_TRUE(window->state().minimized);

  menus->OnMenuRoleActivated(MenuRole::kCopy);
  menus->OnMenuRoleActivated(MenuRole::kClose);
  EventLoop::Current()->RunUntilIdle();
  EXPECT_TRUE(Contains(js.messages, "momo/textinput:command"));
  EXPECT_TRUE(Contains(js.questions, "momo/window:closeRequested"));

  menus->OnMenuRoleActivated(MenuRole::kQuit);
  EXPECT_EQ(app->Run(), 0);
  EXPECT_TRUE(Contains(js.questions, "momo/app:beforeQuit"));
  EXPECT_EQ(state_->menus->delegate, nullptr) << "the channel detaches from the platform menus on teardown";
}

// 平台服务通道与其他宿主通道是同一套实现(HostChannel): 方法按注册表分发, 未知方法回复 unimplemented。
TEST_F(ApplicationTest, PlatformServiceChannelsDispatchAndReportUnknownMethods) {
  WriteBundle();
  auto app = MakeApp();
  ASSERT_TRUE(LaunchAndWait(*app).ok());
  Application* raw = app.get();
  std::string clipboard;
  std::string unknown_clipboard, unknown_text_input;
  bool url_ok = false;
  SendFromJs(raw, "momo/clipboard", {"setText", engine::EncodableValue("copied")},
             [&, raw](StatusOr<engine::EncodableValue> result) {
    EXPECT_TRUE(result.ok()) << result.status();
    SendFromJs(raw, "momo/clipboard", {"getText", engine::EncodableValue()},
               [&, raw](StatusOr<engine::EncodableValue> result) {
      if (result.ok()) clipboard = result->AsString().value_or("");
      SendFromJs(raw, "momo/clipboard", {"nope", engine::EncodableValue()},
                 [&, raw](StatusOr<engine::EncodableValue> result) {
        unknown_clipboard = std::string(result.status().message());
        SendFromJs(raw, "momo/textinput", {"nope", engine::EncodableValue()},
                   [&, raw](StatusOr<engine::EncodableValue> result) {
          unknown_text_input = std::string(result.status().message());
          SendFromJs(raw, "momo/url", {"open", engine::EncodableValue("https://example.com")},
                     [&, raw](StatusOr<engine::EncodableValue> result) {
            url_ok = result.ok();
            raw->task_runners().platform->PostTask([raw] { raw->Exit(0); });
          });
        });
      });
    });
  });
  app->Run();
  EXPECT_EQ(clipboard, "copied");
  EXPECT_EQ(unknown_clipboard.rfind("unimplemented: ", 0), 0u) << unknown_clipboard;
  EXPECT_EQ(unknown_text_input.rfind("unimplemented: ", 0), 0u) << unknown_text_input;
  EXPECT_TRUE(url_ok);
}

// 平台窗口上报的是原始值(可以重复): 去重只在 WindowController 做, 同一个值只通知 JS 一次。
TEST_F(ApplicationTest, RepeatedWindowReportsReachJsOnce) {
  FakeJs js;
  auto app = LaunchWithFakeJs(js);
  Application* raw = app.get();
  PostToPlatform(*app, [&] {
    WindowDelegate* window = state_->window->delegate();
    for (int i = 0; i < 3; ++i) {
      window->OnWindowFrameChanged(Rect{10, 20, 300, 200});
      window->OnWindowControlsChanged(Rect{0, 0, 70, 28});
      window->OnWindowStateChanged(WindowStateFlags{.visible = true, .maximized = true});
    }
    window->OnWindowFrameChanged(Rect{11, 20, 300, 200});
  });
  raw->task_runners().platform->PostDelayedTask([raw] { raw->Exit(0); }, std::chrono::milliseconds(100));
  app->Run();
  EXPECT_EQ(std::count(js.messages.begin(), js.messages.end(), "momo/window:frameChanged"), 2);
  EXPECT_EQ(std::count(js.messages.begin(), js.messages.end(), "momo/window:controlsChanged"), 1);
  EXPECT_EQ(std::count(js.messages.begin(), js.messages.end(), "momo/window:stateChanged"), 1);
}

}  // namespace
}  // namespace momo::embedder
