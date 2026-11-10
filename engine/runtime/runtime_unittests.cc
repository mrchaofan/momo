#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

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

// V8 投递到 isolate 前台 runner 的任务(此处为 FinalizationRegistry 清理)必须在归属的
// ui EventLoop 上执行, 且执行后排空 microtask。
TEST_F(JsRuntimeTest, ForegroundTasksRunOnOwnerEventLoop) {
  Eval("globalThis.cleaned = [];\n"
       "globalThis.registry = new FinalizationRegistry(v => {\n"
       "  cleaned.push(v);\n"
       "  Promise.resolve().then(() => cleaned.push('micro'));\n"
       "});\n"
       "(() => { registry.register({}, 'held'); })();");
  runtime_->impl().isolate()->LowMemoryNotification();  // 完整 GC: 回收对象并投递清理任务
  EXPECT_EQ(Eval("cleaned.join(',')"), "") << "cleanup must not run synchronously inside GC";
  loop_.RunUntilIdle();
  EXPECT_EQ(Eval("cleaned.join(',')"), "held,micro");
}

}  // namespace
}  // namespace momo::engine::runtime

namespace momo::engine::runtime {
namespace {

// 冷启动优化: 第二次运行同一文件模块时使用 V8 code cache;源码变化后缓存失效重建。
TEST(JsRuntimeCodeCacheTest, SecondRunUsesCodeCache) {
  momo::testing::ScopedTestEventLoop loop;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("momo_code_cache_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::filesystem::path main = dir / "main.js";
  const std::filesystem::path lib = dir / "lib.js";
  std::ofstream(lib) << "export function f(x) { return x * 2; }\n";
  std::ofstream(main) << "import { f } from './lib.js'; globalThis.r = f(21);\n";
  JsRuntime::Options options;
  options.code_cache_dir = dir / "cache";
  {
    auto rt = JsRuntime::Create(options);
    ASSERT_TRUE(rt.ok());
    ASSERT_TRUE((*rt)->RunMainModule(main).ok());
    EXPECT_EQ((*rt)->impl().code_cache_stats().hits, 0);
    EXPECT_EQ((*rt)->impl().code_cache_stats().written, 2);
  }
  {
    auto rt = JsRuntime::Create(options);
    ASSERT_TRUE(rt.ok());
    ASSERT_TRUE((*rt)->RunMainModule(main).ok());
    EXPECT_EQ(*(*rt)->EvalToString("r"), "42");
    EXPECT_EQ((*rt)->impl().code_cache_stats().hits, 2);
    EXPECT_EQ((*rt)->impl().code_cache_stats().written, 0);
  }
  std::ofstream(lib) << "export function f(x) { return x * 3; }\n";
  {
    auto rt = JsRuntime::Create(options);
    ASSERT_TRUE(rt.ok());
    ASSERT_TRUE((*rt)->RunMainModule(main).ok());
    EXPECT_EQ(*(*rt)->EvalToString("r"), "63");
    EXPECT_EQ((*rt)->impl().code_cache_stats().hits, 1);  // main 未变
    EXPECT_EQ((*rt)->impl().code_cache_stats().written, 1);
  }
  std::filesystem::remove_all(dir);
}

}  // namespace
}  // namespace momo::engine::runtime
