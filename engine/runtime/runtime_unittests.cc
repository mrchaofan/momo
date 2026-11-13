#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "base/testing/scoped_test_event_loop.h"
#include "engine/runtime/js_args.h"
#include "engine/runtime/js_runtime.h"
#include "engine/runtime/js_runtime_impl.h"
#include "gtest/gtest.h"

namespace momo::engine::runtime {
namespace {

class JsRuntimeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto runtime = JsRuntime::Create({});
    ASSERT_TRUE(runtime.ok()) << runtime.status();
    runtime_ = std::move(runtime).value();
  }
  void TearDown() override { runtime_.reset(); }

  std::string Eval(const std::string& source) {
    auto result = runtime_->EvalToString(source);
    EXPECT_TRUE(result.ok()) << result.status();
    return result.ok() ? *result : std::string();
  }

  // 驱动 ui 线程的 EventLoop(uv 事件由 poll 线程投递到这里)直到 done 成立或超时。
  bool RunUntil(const std::function<bool()>& done,
                std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      loop_.RunUntilIdle();
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
  }

  // 临时目录(每个测试独立)。
  std::filesystem::path TempDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("momo_runtime_" + name + "_" +
                      std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
  }

  momo::testing::ScopedTestEventLoop loop_;  // JsRuntime 归属当前(ui)线程
  std::unique_ptr<JsRuntime> runtime_;
};

TEST_F(JsRuntimeTest, EvaluatesScriptsAndIntl) {
  EXPECT_EQ(Eval("[1, 2, 3].map(x => x * 2).join(',')"), "2,4,6");
  EXPECT_EQ(Eval("new Intl.NumberFormat('en-US').format(1234.5)"), "1,234.5");
}

TEST_F(JsRuntimeTest, ExceptionsBecomeStatusWithStack) {
  auto result = runtime_->EvalToString("function boom() { throw new TypeError('bad'); } boom();",
                                       "test.js");
  ASSERT_FALSE(result.ok());
  const std::string message = result.status().message();
  EXPECT_NE(message.find("TypeError: bad"), std::string::npos) << message;
  EXPECT_NE(message.find("boom"), std::string::npos) << "stack should name the function";
}

TEST_F(JsRuntimeTest, ModuleImportsNativeModule) {
  runtime_->impl().RegisterNativeModule(
      "momo:test", [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                      v8::Local<v8::Object> exports) {
        exports->Set(context, V8String(isolate, "answer"), v8::Number::New(isolate, 42)).Check();
      });
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("import { answer } from 'momo:test';\n"
                                    "globalThis.result = answer + 1;",
                                    "main.js")
                  .ok());
  EXPECT_EQ(Eval("result"), "43");
}

TEST_F(JsRuntimeTest, UnknownNativeModuleFailsToLink) {
  Status status = runtime_->RunModuleSource("import x from 'momo:nope';", "main.js");
  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("momo:nope"), std::string::npos) << status;
}

TEST_F(JsRuntimeTest, MainModuleResolvesRelativeImports) {
  const auto dir = std::filesystem::temp_directory_path() / "momo_runtime_test_modules";
  std::filesystem::create_directories(dir / "lib");
  std::ofstream(dir / "lib" / "math.js") << "export const square = (x) => x * x;\n";
  std::ofstream(dir / "lib" / "util.js") << "import { square } from './math.js';\n"
                                            "export const quad = (x) => square(square(x));\n";
  std::ofstream(dir / "main.js") << "import { quad } from './lib/util.js';\n"
                                    "globalThis.result = quad(2);\n";
  ASSERT_TRUE(runtime_->RunMainModule(dir / "main.js").ok());
  EXPECT_EQ(Eval("result"), "16");
  std::filesystem::remove_all(dir);
}

TEST_F(JsRuntimeTest, MissingRelativeImportReportsPath) {
  Status status = runtime_->RunModuleSource("import './nope.js';", "main.js", "/tmp");
  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("nope.js"), std::string::npos) << status;
}

TEST_F(JsRuntimeTest, TopLevelThrowAndRejectionFailTheModule) {
  Status thrown = runtime_->RunModuleSource("throw new Error('top-level');", "a.js");
  ASSERT_FALSE(thrown.ok());
  EXPECT_NE(thrown.message().find("top-level"), std::string::npos) << thrown;

  Status rejected = runtime_->RunModuleSource("await Promise.reject(new Error('async-boom'));",
                                              "b.js");
  ASSERT_FALSE(rejected.ok());
  EXPECT_NE(rejected.message().find("async-boom"), std::string::npos) << rejected;
}

TEST_F(JsRuntimeTest, SyntaxErrorFailsCompilation) {
  Status status = runtime_->RunModuleSource("let = ;", "bad.js");
  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("SyntaxError"), std::string::npos) << status;
}

TEST_F(JsRuntimeTest, MicrotasksAreDrainedAfterModuleRun) {
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("globalThis.order = [];\n"
                                    "Promise.resolve().then(() => order.push('micro'));\n"
                                    "order.push('sync');",
                                    "m.js")
                  .ok());
  EXPECT_EQ(Eval("order.join(',')"), "sync,micro");
}

// V8 前台任务(此处为 FinalizationRegistry 清理)经 Node 平台的 uv 句柄, 由 poll 线程交给 ui 线程执行;
// 不在 GC 内同步执行。
TEST_F(JsRuntimeTest, ForegroundTasksRunOnTheUiThreadThroughUv) {
  Eval("globalThis.cleaned = [];\n"
       "globalThis.registry = new FinalizationRegistry(v => {\n"
       "  cleaned.push(v);\n"
       "  Promise.resolve().then(() => cleaned.push('micro'));\n"
       "});\n"
       "(() => { registry.register({}, 'held'); })();");
  runtime_->impl().isolate()->LowMemoryNotification();  // 完整 GC: 回收对象并投递清理任务
  EXPECT_EQ(Eval("cleaned.join(',')"), "") << "cleanup must not run synchronously inside GC";
  EXPECT_TRUE(RunUntil([&] { return Eval("cleaned.join(',')") == "held,micro"; }))
      << Eval("cleaned.join(',')");
}

// ---- Node.js 集成(docs/04 §1;对照 Electron NodeBindings)----

// Node 的定时器: 由 poll 线程等到期, 在 ui 线程执行(不需要任何 momo 事件触发)。
TEST_F(JsRuntimeTest, NodeTimersFireOnTheUiThread) {
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("globalThis.fired = [];\n"
                                    "setTimeout(() => fired.push('timeout'), 20);\n"
                                    "setImmediate(() => fired.push('immediate'));",
                                    "timers.mjs")
                  .ok());
  EXPECT_TRUE(RunUntil([&] { return Eval("fired.join(',')") == "immediate,timeout"; }))
      << Eval("fired.join(',')");
}

// 从 momo 进入 JS(此处为 EvalToString)时新增的定时器: poll 线程被唤醒并按新超时等待
// (Electron 靠 libuv 补丁 uv_loop_interrupt, momo 在 DrainMicrotasks 后唤醒)。
TEST_F(JsRuntimeTest, TimersAddedFromMomoCallbacksWakeThePollThread) {
  Eval("globalThis.late = false; setTimeout(() => { late = true; }, 10); 0");
  EXPECT_TRUE(RunUntil([&] { return Eval("String(late)") == "true"; }, std::chrono::seconds(2)));
}

// Node 的异步 IO: fs.promises 在 uv 线程池完成, 回调在 ui 线程。
TEST_F(JsRuntimeTest, NodeFsPromisesComplete) {
  const auto dir = TempDir("fs");
  std::ofstream(dir / "data.txt") << "hello from fs";
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("import { readFile } from 'node:fs/promises';\n"
                                    "globalThis.text = null;\n"
                                    "readFile(" + std::string("'") + (dir / "data.txt").string() +
                                        "', 'utf8').then(t => { text = t; });",
                                    "fs.mjs")
                  .ok());
  EXPECT_TRUE(RunUntil([&] { return Eval("String(text)") == "hello from fs"; }));
  std::filesystem::remove_all(dir);
}

// 从 momo 进入 JS 之后按 Node 语义排空: process.nextTick 先于 Promise 微任务。
TEST_F(JsRuntimeTest, DrainRunsNextTicksBeforeMicrotasks) {
  Eval("globalThis.order = [];\n"
       "Promise.resolve().then(() => order.push('promise'));\n"
       "process.nextTick(() => order.push('tick'));\n"
       "order.push('sync'); 0");
  EXPECT_EQ(Eval("order.join(',')"), "sync,tick,promise");
}

// 从 momo 调用 JS 函数(帧、输入、通道回调)与 node::MakeCallback 相同: 返回前已按 Node 语义排空
// nextTick 与微任务, 调用方不必另外排空;嵌套调用由最外层排空。
TEST_F(JsRuntimeTest, CallsDrainNextTicksAndMicrotasksBeforeReturning) {
  Eval("globalThis.order = [];\n"
       "globalThis.f = () => {\n"
       "  Promise.resolve().then(() => order.push('promise'));\n"
       "  process.nextTick(() => order.push('tick'));\n"
       "  order.push('call');\n"
       "}; 0");
  JsRuntimeImpl& impl = runtime_->impl();
  v8::HandleScope handle_scope(impl.isolate());
  v8::Local<v8::Context> context = impl.context();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Value> f =
      context->Global()->Get(context, v8::String::NewFromUtf8Literal(impl.isolate(), "f")).ToLocalChecked();
  ASSERT_TRUE(impl.Call(f.As<v8::Function>(), v8::Undefined(impl.isolate()), 0, nullptr, "f").ok());
  v8::Local<v8::Value> order =
      context->Global()->Get(context, v8::String::NewFromUtf8Literal(impl.isolate(), "order")).ToLocalChecked();
  v8::String::Utf8Value text(impl.isolate(), order);
  EXPECT_STREQ(*text, "call,tick,promise");
}

// 顶层 await 等待 Node IO: 入口经 uv_run 泵送, 返回时求值已完成。
TEST_F(JsRuntimeTest, EntryTopLevelAwaitIsPumpedThroughUv) {
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("import { setTimeout as sleep } from 'node:timers/promises';\n"
                                    "await sleep(20);\n"
                                    "globalThis.done = 'yes';",
                                    "tla.mjs")
                  .ok());
  EXPECT_EQ(Eval("String(globalThis.done)"), "yes");
}

// 顶层 await 等待 uv 之外的事(这里是永不 resolve 的 Promise): uv 无事可做时停止泵送, 不卡住启动。
TEST_F(JsRuntimeTest, EntryWaitingOnNonUvWorkDoesNotBlock) {
  ASSERT_TRUE(runtime_->RunModuleSource("globalThis.started = true; await new Promise(() => {});",
                                        "pending.mjs")
                  .ok());
  EXPECT_EQ(Eval("String(started)"), "true");
}

// npm 包: 按 Node 规则从 node_modules 解析(CJS 包以默认导出导入)。
TEST_F(JsRuntimeTest, ImportsPackagesFromNodeModules) {
  const auto dir = TempDir("pkg");
  std::filesystem::create_directories(dir / "node_modules" / "greet");
  std::ofstream(dir / "node_modules" / "greet" / "package.json")
      << R"({"name":"greet","version":"1.0.0","main":"index.js"})";
  std::ofstream(dir / "node_modules" / "greet" / "index.js")
      << "module.exports = (n) => 'hi ' + n;\n";
  std::ofstream(dir / "main.js") << "import greet from 'greet';\n"
                                    "import { createRequire } from 'node:module';\n"
                                    "const require = createRequire(import.meta.url);\n"
                                    "globalThis.out = greet('momo') + '/' + require('greet')('cjs');\n";
  ASSERT_TRUE(runtime_->RunMainModule(dir / "main.js").ok());
  EXPECT_EQ(Eval("out"), "hi momo/hi cjs");
  std::filesystem::remove_all(dir);
}

// 未捕获的异常(uv 回调里抛出)只记录, 不退出进程, 之后的定时器照常执行。
TEST_F(JsRuntimeTest, UncaughtExceptionsDoNotKillTheProcess) {
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("globalThis.after = false;\n"
                                    "setTimeout(() => { throw new Error('boom in timer'); }, 1);\n"
                                    "setTimeout(() => { after = true; }, 30);",
                                    "throw.mjs")
                  .ok());
  EXPECT_TRUE(RunUntil([&] { return Eval("String(after)") == "true"; }));
}

}  // namespace
}  // namespace momo::engine::runtime

namespace momo::engine::runtime {
namespace {

// Node 编译缓存(module.enableCompileCache): 第一次运行后写入缓存目录, 第二次运行照常。
TEST(JsRuntimeCodeCacheTest, CompileCacheIsWritten) {
  momo::testing::ScopedTestEventLoop loop;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("momo_code_cache_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::filesystem::path main = dir / "main.js";
  std::ofstream(dir / "lib.js") << "export function f(x) { return x * 2; }\n";
  std::ofstream(main) << "import { f } from './lib.js'; globalThis.r = f(21);\n";
  JsRuntime::Options options;
  options.code_cache_dir = dir / "cache";
  for (int run = 0; run < 2; ++run) {
    auto rt = JsRuntime::Create(options);
    ASSERT_TRUE(rt.ok()) << rt.status();
    ASSERT_TRUE((*rt)->RunMainModule(main).ok());
    EXPECT_EQ(*(*rt)->EvalToString("r"), "42");
  }
  size_t files = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir / "cache")) {
    files += entry.is_regular_file() ? 1 : 0;
  }
  EXPECT_GT(files, 0u) << "Node should have written its compile cache";
  std::filesystem::remove_all(dir);
}

// 反复创建/销毁运行时(hot restart): 每次都是全新的 Node 环境, poll 线程与 uv 循环干净退出。
TEST(JsRuntimeLifecycleTest, RepeatedCreateAndDestroy) {
  momo::testing::ScopedTestEventLoop loop;
  for (int i = 0; i < 3; ++i) {
    auto rt = JsRuntime::Create({});
    ASSERT_TRUE(rt.ok()) << rt.status();
    ASSERT_TRUE((*rt)->RunModuleSource("setInterval(() => {}, 5); globalThis.v = " +
                                           std::to_string(i) + ";",
                                       "loop.mjs")
                    .ok());
    EXPECT_EQ(*(*rt)->EvalToString("String(v)"), std::to_string(i));
    loop.RunUntilIdle();
  }
}

}  // namespace
}  // namespace momo::engine::runtime
