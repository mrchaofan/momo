// ExtensionHost + ext:* JS 胶水的端到端测试: 加载真实的示例扩展动态库(extensions/examples)。

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "engine/extension/extension_host.h"
#include "engine/runtime/js_runtime.h"
#include "engine/ui/ui_bindings.h"
#include "gtest/gtest.h"

namespace momo::engine {
namespace {

TEST(ExtensionManifestTest, ParsesEntriesAndRejectsBadInput) {
  auto entries = ParseExtensionManifest(R"({"extensions": [
      {"id": "a.b", "library": "libx.dylib", "specifier": "ext:x", "capabilities": ["texture"]}]})");
  ASSERT_TRUE(entries.ok()) << entries.status();
  ASSERT_EQ(entries->size(), 1u);
  EXPECT_EQ((*entries)[0].id, "a.b");
  EXPECT_EQ((*entries)[0].capabilities, std::vector<std::string>{"texture"});
  EXPECT_FALSE(ParseExtensionManifest("{").ok());
  EXPECT_FALSE(ParseExtensionManifest(R"({"extensions": [{"id": "x"}]})").ok());
  EXPECT_FALSE(ParseExtensionManifest(R"({"nope": []})").ok());
}

class FakeClient final : public ui::WindowClient {
 public:
  void ScheduleFrame() override {}
  void Render(std::unique_ptr<graphics::LayerTree>) override {}
  void SetTitle(ui::ViewId, std::string) override {}
  void SetCursor(ui::ViewId, platform::SystemCursor) override {}
  void SetMinSize(ui::ViewId, Size) override {}
  platform::ViewportMetrics metrics(ui::ViewId) const override { return {{100, 100}, 1}; }
  bool focused(ui::ViewId) const override { return false; }
};

class ExtensionHostTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("momo_ext_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
            "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    bindings_.reset();
    runtime_.reset();
    if (host_) host_->DetachAll();
    loop_.RunUntilIdle();
    host_.reset();
    std::filesystem::remove_all(dir_);
  }

  void WriteManifest(const std::string& json) {
    std::ofstream(dir_ / "extensions.manifest") << json;
  }
  void CopyLib(const char* from, const std::string& name) {
    std::filesystem::copy_file(from, dir_ / name, std::filesystem::copy_options::overwrite_existing);
  }

  void CreateHost() {
    ExtensionHost::Environment env;
    env.runners = {"test", loop_.task_runner(), loop_.task_runner(), loop_.task_runner(),
                   loop_.task_runner()};
    env.hub = hub_;
    env.textures = textures_;
    env.extensions_dir = dir_;
    env.request_frame = [this] { frame_requests_++; };
    env.platform_views = views_;
    host_ = std::make_unique<ExtensionHost>(std::move(env));
  }

  // 安装 JS 运行时(ext:* 模块在此注册)。
  void StartJs() {
    auto runtime = runtime::JsRuntime::Create({});
    ASSERT_TRUE(runtime.ok());
    runtime_ = std::move(runtime).value();
    ui::AppHost app;
    app.ui_runner = loop_.task_runner();
    app.io_runner = loop_.task_runner();
    app.hub = hub_;
    app.extensions = host_.get();
    app.textures = textures_;
    bindings_ = ui::UiBindings::Install(*runtime_, client_, app);
  }
  std::string Eval(const std::string& js) {
    auto r = runtime_->EvalToString(js);
    EXPECT_TRUE(r.ok()) << r.status();
    return r.ok() ? *r : "";
  }

  momo::testing::ScopedTestEventLoop loop_;
  std::filesystem::path dir_;
  std::shared_ptr<MessageChannelHub> hub_ = std::make_shared<MessageChannelHub>();
  std::shared_ptr<graphics::TextureRegistry> textures_ =
      std::make_shared<graphics::TextureRegistry>();
  std::shared_ptr<PlatformViewRegistry> views_ = std::make_shared<PlatformViewRegistry>();
  std::unique_ptr<ExtensionHost> host_;
  int frame_requests_ = 0;
  FakeClient client_;
  std::unique_ptr<runtime::JsRuntime> runtime_;
  std::unique_ptr<ui::UiBindings> bindings_;
};

TEST_F(ExtensionHostTest, MissingManifestMeansNoExtensions) {
  CreateHost();
  EXPECT_TRUE(host_->LoadFromManifest().ok());
  EXPECT_TRUE(host_->attached_ids().empty());
  EXPECT_TRUE(host_->errors().empty());
}

TEST_F(ExtensionHostTest, BadEntriesAreIsolated) {
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
TEST_F(ExtensionHostTest, HelloExtensionEndToEnd) {
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

TEST_F(ExtensionHostTest, TextureExtensionPushesFramesViaFrameCallbacks) {
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

// 进程内扩展: on_detach 之后对宿主 API 的调用被拒绝(而不是访问已释放状态)。
const MomoHostApi* g_api = nullptr;
MomoEngineRef g_engine_ref = nullptr;
int g_detached = 0;

bool InitInProcess(uint32_t version, MomoExtension* out) {
  out->id = "test.inprocess";
  out->on_attach = [](void*, const MomoHostApi* api, MomoEngineRef engine) {
    g_api = api;
    g_engine_ref = engine;
  };
  out->on_detach = [](void*) { g_detached++; };
  return version >= 1;
}

TEST_F(ExtensionHostTest, CallsAfterDetachAreRejected) {
  CreateHost();
  ASSERT_TRUE(host_->AddExtension("test.inprocess", &InitInProcess).ok());
  ASSERT_NE(g_api, nullptr);
  EXPECT_EQ(g_api->api_version, MOMO_EXT_API_VERSION);
  EXPECT_EQ(g_api->register_module(g_engine_ref, "notext", nullptr, nullptr), kMomoErrInvalidArgs);
  EXPECT_EQ(g_api->channel_set_handler(g_engine_ref, "momo/clipboard", kMomoRunnerUi,
                                       [](void*, const uint8_t*, size_t, uint64_t) {}, nullptr),
            kMomoErrInvalidArgs)
      << "momo/* channels are reserved";
  void* handle = nullptr;
  EXPECT_EQ(g_api->get_window_handle(g_engine_ref, 0, &handle), kMomoErrNotFound);
  host_->DetachAll();
  EXPECT_EQ(g_detached, 1);
  EXPECT_EQ(g_api->post_task(g_engine_ref, kMomoRunnerUi, [](void*) {}, nullptr), kMomoErrNotFound);
  EXPECT_FALSE(host_->AddExtension("test.inprocess", &InitInProcess).ok());
}

// 回调线程规则: 宿主只在扩展注册回调的线程上调用它。
TEST_F(ExtensionHostTest, CallbacksOnlyRunOnRegisteringThread) {
  CreateHost();
  ASSERT_TRUE(host_->AddExtension("test.inprocess", &InitInProcess).ok());

  // 帧回调在 ui 线程执行, 因此只能在 ui 线程注册(本测试中 ui runner 就是当前线程)。
  uint64_t frame_id = 0;
  MomoStatus off_ui = kMomoOk;
  std::thread([&] {
    off_ui = g_api->add_frame_callback(g_engine_ref, [](void*) {}, nullptr, &frame_id);
  }).join();
  EXPECT_EQ(off_ui, kMomoErrWrongThread);
  ASSERT_EQ(g_api->add_frame_callback(g_engine_ref, [](void*) {}, nullptr, &frame_id), kMomoOk);
  EXPECT_EQ(g_api->remove_frame_callback(g_engine_ref, frame_id), kMomoOk);

  // acquire/release 纹理: 只有注册线程上的通知会触发回调。
  static int acquired_off_thread = 0;
  static std::thread::id registering_thread;
  registering_thread = std::this_thread::get_id();
  MomoTextureDesc desc{};
  desc.struct_size = sizeof(desc);
  desc.kind = kMomoTexturePixelBuffer;
  desc.acquire = [](void*, uint32_t* w, uint32_t* h) -> const void* {
    static const uint8_t pixel[4] = {1, 2, 3, 255};
    if (std::this_thread::get_id() != registering_thread) acquired_off_thread++;
    *w = 1;
    *h = 1;
    return pixel;
  };
  int64_t texture = 0;
  ASSERT_EQ(g_api->texture_register(g_engine_ref, &desc, &texture), kMomoOk);
  MomoStatus other_thread = kMomoOk;
  std::thread([&] { other_thread = g_api->texture_mark_frame_available(g_engine_ref, texture); })
      .join();
  EXPECT_EQ(other_thread, kMomoErrWrongThread);
  EXPECT_EQ(g_api->texture_mark_frame_available(g_engine_ref, texture), kMomoOk);
  EXPECT_EQ(acquired_off_thread, 0);
  EXPECT_NE(textures_->Get(texture)->latest(), nullptr);
  EXPECT_EQ(g_api->texture_unregister(g_engine_ref, texture), kMomoOk);
  host_->DetachAll();
}


// ---- 平台视图工厂(C ABI v1 追加字段, docs/05 §9)----

std::vector<std::string> g_view_log;
const MomoHostApi* g_view_api = nullptr;
MomoEngineRef g_view_engine = nullptr;
MomoStatus g_view_register = kMomoErrInternal;
int g_native_view = 0;

void* CreateTestView(void*, int64_t id, const uint8_t* params, size_t len) {
  g_view_log.push_back("create:" + std::to_string(id) + ":" +
                       std::string(reinterpret_cast<const char*>(params), len));
  return &g_native_view;
}
void DisposeTestView(void*, int64_t id, void* native) {
  g_view_log.push_back("dispose:" + std::to_string(id) + (native == &g_native_view ? "" : ":bad"));
}

bool InitViewExtension(uint32_t version, MomoExtension* out) {
  out->id = "test.views";
  out->on_attach = [](void*, const MomoHostApi* api, MomoEngineRef engine) {
    g_view_api = api;
    g_view_engine = engine;
    const MomoPlatformViewFactory factory{sizeof(MomoPlatformViewFactory), &CreateTestView,
                                          &DisposeTestView, nullptr};
    g_view_register = api->platform_view_register_factory(engine, "test.view", &factory);
  };
  out->on_detach = [](void*) { g_view_log.push_back("detach"); };
  return version >= 1;
}

TEST_F(ExtensionHostTest, PlatformViewFactoryViaCAbi) {
  g_view_log.clear();
  CreateHost();
  ASSERT_TRUE(host_->AddExtension("test.views", &InitViewExtension).ok());
  ASSERT_GE(g_view_api->struct_size, offsetof(MomoHostApi, platform_view_get) + sizeof(void*));
  EXPECT_EQ(g_view_register, kMomoOk);
  EXPECT_TRUE(views_->HasFactory("test.view"));
  // 重复类型、保留前缀、残缺工厂都被拒绝。
  const MomoPlatformViewFactory factory{sizeof(MomoPlatformViewFactory), &CreateTestView,
                                        &DisposeTestView, nullptr};
  EXPECT_EQ(g_view_api->platform_view_register_factory(g_view_engine, "test.view", &factory),
            kMomoErrInvalidArgs);
  EXPECT_EQ(g_view_api->platform_view_register_factory(g_view_engine, "momo/view", &factory),
            kMomoErrInvalidArgs);
  const MomoPlatformViewFactory broken{sizeof(MomoPlatformViewFactory), nullptr, &DisposeTestView,
                                       nullptr};
  EXPECT_EQ(g_view_api->platform_view_register_factory(g_view_engine, "test.broken", &broken),
            kMomoErrInvalidArgs);

  // 引擎(Shell)按 JS 请求创建视图;扩展按 id 取回原生视图。
  ASSERT_TRUE(views_->Create(7, "test.view", {'u', 'r', 'l'}).ok());
  ASSERT_TRUE(views_->Create(8, "test.view", {}).ok());
  void* native = nullptr;
  EXPECT_EQ(g_view_api->platform_view_get(g_view_engine, 7, &native), kMomoOk);
  EXPECT_EQ(native, &g_native_view);
  EXPECT_EQ(g_view_api->platform_view_get(g_view_engine, 99, &native), kMomoErrNotFound);
  std::thread other([] {
    void* out = nullptr;
    EXPECT_EQ(g_view_api->platform_view_get(g_view_engine, 7, &out), kMomoErrWrongThread);
  });
  other.join();
  ASSERT_TRUE(views_->Dispose(8).ok());

  // detach: 残留视图先 dispose, 再 on_detach;之后类型不可再用。
  host_->DetachAll();
  EXPECT_EQ(g_view_log, (std::vector<std::string>{"create:7:url", "create:8:", "dispose:8",
                                                  "dispose:7", "detach"}));
  EXPECT_FALSE(views_->HasFactory("test.view"));
  EXPECT_EQ(views_->size(), 0u);
}

#if defined(MOMO_EXT_WEBVIEW_LIB)
// 真实的 WebView 示例扩展: 加载后注册 "momo.webview" 视图类型(不创建视图: 测试进程没有 AppKit 主循环)。
TEST_F(ExtensionHostTest, WebViewExtensionRegistersItsViewType) {
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
