// 示例扩展(extensions/examples)的端到端测试: 经 manifest 加载真实的扩展动态库, 只经 C ABI 与宿主交互。
// 放在 extensions/ 而不是 engine/: 引擎的测试不依赖示例代码(依赖方向 examples → engine)。

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "engine/extension/testing/extension_host_fixture.h"

namespace momo::engine {
namespace {

// 测试套件名与引擎的 ExtensionHostTest 区分(CTest 用例名全局唯一)。
using ExampleExtensionTest = testing::ExtensionHostTest;

TEST_F(ExampleExtensionTest, BadEntriesAreIsolated) {
  CopyLib(MOMO_EXT_HELLO_LIB, "libext_hello.dylib");
  WriteManifest(R"({"extensions": [
      {"id": "wrong.id", "library": "libext_hello.dylib"},
      {"id": "x.missing", "library": "nope.dylib"},
      {"id": "x.escape", "library": "../../etc/evil.dylib"},
      {"id": "momo.example.hello", "library": "libext_hello.dylib"}]})");
  CreateHost();
  ASSERT_TRUE(host_->LoadFromManifest().ok());
  EXPECT_EQ(host_->attached_ids(), std::vector<std::string>{"momo.example.hello"});
  const std::vector<std::string> errors = host_->errors();
  ASSERT_EQ(errors.size(), 3u);
  EXPECT_NE(errors[0].find("id mismatch"), std::string::npos);
  EXPECT_NE(errors[2].find("escapes"), std::string::npos);
}

// 纯 C 扩展经 C ABI 暴露: 同步函数、字符串/字节、worker 线程上的 Promise、消息通道。
TEST_F(ExampleExtensionTest, HelloExtensionEndToEnd) {
  CopyLib(MOMO_EXT_HELLO_LIB, "libext_hello.dylib");
  WriteManifest(R"({"extensions": [{"id": "momo.example.hello", "library": "libext_hello.dylib",
                                     "specifier": "ext:hello"}]})");
  CreateHost();
  ASSERT_TRUE(host_->LoadFromManifest().ok());
  ASSERT_EQ(host_->modules().size(), 1u);
  StartJs();
  ASSERT_TRUE(runtime_->RunModuleSource(R"JS(
    import { native, add } from 'ext:hello';
    import { send } from 'momo:services';
    globalThis.sync = [add(2, 40), native.greet('世界'),
                       Array.from(new Uint8Array(native.reverseBytes(new Uint8Array([1, 2, 3]))))
                      ].join('|');
    try { add('x'); } catch (e) { globalThis.err = String(e.message); }
    const enc = (s) => new Uint8Array([...s].map((c) => c.charCodeAt(0)));
    native.hashAsync(enc('momo')).then((buf) => {
      const b = new Uint8Array(buf);
      let hex = '';
      for (let i = 7; i >= 0; i--) hex += b[i].toString(16).padStart(2, '0');
      globalThis.hash = hex;
    });
    send('hello/upper', enc('abc!').buffer).then((buf) => {
      globalThis.upper = String.fromCharCode(...new Uint8Array(buf));
    });
  )JS", "app.js").ok());
  EXPECT_EQ(Eval("sync"), "42|Hello, 世界!|3,2,1");
  EXPECT_EQ(Eval("err"), "add(a, b) expects two numbers");
  // worker pool 上的任务完成后投递回 ui(测试 loop)。
  for (int i = 0; i < 200 && (Eval("typeof hash") != "string" || Eval("typeof upper") != "string");
       ++i) {
    loop_.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // FNV-1a 64("momo")
  uint64_t h = 1469598103934665603ULL;
  for (char c : std::string("momo")) {
    h ^= static_cast<uint8_t>(c);
    h *= 1099511628211ULL;
  }
  char expected[17];
  std::snprintf(expected, sizeof(expected), "%016llx", static_cast<unsigned long long>(h));
  EXPECT_EQ(Eval("hash"), expected);
  EXPECT_EQ(Eval("upper"), "ABC!");
}

TEST_F(ExampleExtensionTest, TextureExtensionPushesFramesViaFrameCallbacks) {
  CopyLib(MOMO_EXT_TEXTURE_LIB, "libext_texture.dylib");
  WriteManifest(R"({"extensions": [{"id": "momo.example.texture",
                                     "library": "libext_texture.dylib"}]})");
  CreateHost();
  ASSERT_TRUE(host_->LoadFromManifest().ok());
  StartJs();
  ASSERT_TRUE(runtime_->RunModuleSource(R"JS(
    import { start } from 'ext:texture';
    globalThis.textureId = start(32, 16);
  )JS", "app.js").ok());
  const int64_t id = std::stoll(Eval("textureId"));
  ASSERT_NE(textures_->Get(id), nullptr);
  loop_.RunUntilIdle();  // request_frame 被投递到 platform runner 上执行
  EXPECT_GE(frame_requests_, 1) << "registering a frame callback requests frames";
  EXPECT_TRUE(host_->has_frame_callbacks());
  for (int i = 0; i < 200 && textures_->Get(id)->latest() == nullptr; ++i) {
    host_->RunFrameCallbacks();  // 引擎每帧调用(ui 线程)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::shared_ptr<const graphics::Image> frame = textures_->Get(id)->latest();
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->width(), 32);
  EXPECT_EQ(frame->height(), 16);
  Eval("0");
  ASSERT_TRUE(runtime_->RunModuleSource("import { stop } from 'ext:texture'; stop();", "s.js").ok());
  EXPECT_FALSE(host_->has_frame_callbacks());
  EXPECT_EQ(textures_->Get(id), nullptr);
}

#if defined(MOMO_EXT_WEBVIEW_LIB)
// 真实的 WebView 示例扩展: 加载后注册 "momo.webview" 视图类型(不创建视图: 测试进程没有 AppKit 主循环)。
TEST_F(ExampleExtensionTest, WebViewExtensionRegistersItsViewType) {
  CopyLib(MOMO_EXT_WEBVIEW_LIB, "libext_webview.dylib");
  WriteManifest(R"({"extensions": [{"id": "momo.example.webview", "library": "libext_webview.dylib"}]})");
  CreateHost();
  ASSERT_TRUE(host_->LoadFromManifest().ok());
  EXPECT_EQ(host_->attached_ids(), std::vector<std::string>{"momo.example.webview"});
  EXPECT_TRUE(host_->errors().empty());
  EXPECT_TRUE(views_->HasFactory("momo.webview"));
}
#endif

}  // namespace
}  // namespace momo::engine
