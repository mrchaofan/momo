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
  runtime_->RegisterNativeModule(
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
  runtime_->isolate()->LowMemoryNotification();  // 完整 GC: 回收对象并投递清理任务
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
                                    "readFile(" + std::string("'") + (dir / "data.txt").generic_string() +
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
  v8::HandleScope handle_scope(runtime_->isolate());
  v8::Local<v8::Context> context = runtime_->context();
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Value> f =
      context->Global()->Get(context, v8::String::NewFromUtf8Literal(runtime_->isolate(), "f")).ToLocalChecked();
  ASSERT_TRUE(runtime_->Call(f.As<v8::Function>(), v8::Undefined(runtime_->isolate()), 0, nullptr, "f").ok());
  v8::Local<v8::Value> order =
      context->Global()->Get(context, v8::String::NewFromUtf8Literal(runtime_->isolate(), "order")).ToLocalChecked();
  v8::String::Utf8Value text(runtime_->isolate(), order);
  EXPECT_STREQ(*text, "call,tick,promise");
}

// 入口不等待顶层 await(同 Electron): 同步部分执行完即返回, 余下的求值在事件循环里继续。
TEST_F(JsRuntimeTest, EntryDoesNotWaitForTopLevelAwait) {
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("import { setTimeout as sleep } from 'node:timers/promises';\n"
                                    "globalThis.started = true;\n"
                                    "await sleep(20);\n"
                                    "globalThis.done = 'yes';",
                                    "tla.mjs")
                  .ok());
  EXPECT_EQ(Eval("String(globalThis.started)"), "true");
  EXPECT_EQ(Eval("String(globalThis.done)"), "undefined") << "the entry was awaited";
  EXPECT_TRUE(RunUntil([&] { return Eval("String(globalThis.done)") == "yes"; }));
}

// 顶层 await 等待本线程的引擎任务(平台通道应答就是这样到达的), 同时还有未到期的 uv 定时器: 入口必须
// 立即返回。等到 uv 无事可做才返回的话, 这段时间引擎任务无法执行, 启动会卡住(有 setInterval 时永远卡住)。
TEST_F(JsRuntimeTest, EntryWaitingOnEngineWorkReturnsWhileUvIsBusy) {
  const auto begin = std::chrono::steady_clock::now();
  ASSERT_TRUE(runtime_
                  ->RunModuleSource("globalThis.resolved = false;\n"
                                    "globalThis.timer = setInterval(() => {}, 1000);\n"
                                    "await new Promise((resolve) => { globalThis.reply = resolve; });\n"
                                    "globalThis.resolved = true;\n"
                                    "clearInterval(globalThis.timer);",
                                    "channel.mjs")
                  .ok());
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(800));
  EXPECT_EQ(Eval("String(resolved)"), "false");
  // 应答到达(引擎任务调用 JS)后, 余下的求值继续。
  EXPECT_EQ(Eval("reply(); 'sent'"), "sent");
  EXPECT_TRUE(RunUntil([&] { return Eval("String(resolved)") == "true"; }));
}

// 入口同步部分的错误仍然作为启动失败返回;加载失败同样如此。
TEST_F(JsRuntimeTest, EntrySynchronousErrorsFailTheRun) {
  EXPECT_FALSE(runtime_->RunModuleSource("throw new Error('boom at top level');", "throw_sync.mjs").ok());
  EXPECT_FALSE(runtime_->RunModuleSource("import './does_not_exist.mjs';", "missing_import.mjs").ok());
}

// 顶层 await 等待永不 resolve 的 Promise, uv 也无事可做: 不卡住启动。
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

// 运行时自带的 JS 模块: momo:<name> 解析到 <builtin_dir>/<name>/package.json 的 main;它是普通的文件模块(相对
// 导入照常), 多处 import 得到同一份;目录里没有的名字仍是引擎的原生模块。
TEST(JsRuntimeBuiltinTest, MomoSpecifierResolvesToTheBuiltinDirectory) {
  momo::testing::ScopedTestEventLoop loop;  // JsRuntime 归属当前(ui)线程
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "momo_builtin_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "kit" / "lib");
  std::ofstream(dir / "kit" / "package.json") << R"({"type": "module", "main": "lib/main.js"})";
  std::ofstream(dir / "kit" / "lib" / "main.js")
      << "import { base } from './base.js';\n"
         "globalThis.kitLoads = (globalThis.kitLoads ?? 0) + 1;\n"
         "export const answer = base + 2;\n";
  std::ofstream(dir / "kit" / "lib" / "base.js") << "export const base = 40;\n";
  // 依赖 momo:kit 的另一个自带模块: 与应用拿到的是同一份。
  std::filesystem::create_directories(dir / "widget");
  std::ofstream(dir / "widget" / "package.json") << R"({"type": "module"})";
  std::ofstream(dir / "widget" / "index.js")
      << "import { answer } from 'momo:kit';\nexport const doubled = answer * 2;\n";

  JsRuntime::Options options;
  options.builtin_dir = dir;
  auto rt = JsRuntime::Create(options);
  ASSERT_TRUE(rt.ok()) << rt.status();
  (*rt)->RegisterNativeModule(
      "momo:native", [](v8::Isolate* isolate, v8::Local<v8::Context> context,
                        v8::Local<v8::Object> exports) {
        exports->Set(context, V8String(isolate, "seven"), v8::Number::New(isolate, 7)).Check();
      });
  Status status = (*rt)->RunModuleSource(
      "import { answer } from 'momo:kit';\n"
      "import { doubled } from 'momo:widget';\n"
      "import { seven } from 'momo:native';\n"
      "globalThis.r = [answer, doubled, seven, globalThis.kitLoads].join();\n",
      "main.js");
  ASSERT_TRUE(status.ok()) << status;
  EXPECT_EQ(*(*rt)->EvalToString("r"), "42,84,7,1");

  status = (*rt)->RunModuleSource("import 'momo:../kit';", "escape.js");
  EXPECT_FALSE(status.ok()) << "only plain names map into the built-in directory";
  rt->reset();
  std::filesystem::remove_all(dir);
}

// 反复创建/销毁运行时(测试进程里常见): 每次都是全新的 Node 环境, poll 线程与 uv 循环干净退出。
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
