// ExtensionHost + ext:* JS 胶水的测试: 进程内测试扩展经 C ABI 驱动宿主(不依赖 extensions/examples;
// 加载真实示例扩展动态库的端到端用例在 extensions/examples/examples_unittests.cc)。

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/extension/testing/extension_host_fixture.h"
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

using testing::ExtensionHostTest;

// manifest 中的坏条目各自报错, 不影响其他条目(加载真实扩展的用例见 extensions/examples/examples_unittests.cc)。
TEST_F(ExtensionHostTest, BadEntriesAreIsolated) {
  WriteManifest(R"({"extensions": [
      {"id": "x.missing", "library": "nope.dylib"},
      {"id": "x.escape", "library": "../../etc/evil.dylib"}]})");
  CreateHost();
  ASSERT_TRUE(host_->LoadFromManifest().ok());
  EXPECT_TRUE(host_->attached_ids().empty());
  const std::vector<std::string> errors = host_->errors();
  ASSERT_EQ(errors.size(), 2u);
  EXPECT_NE(errors[1].find("escapes"), std::string::npos);
}

TEST_F(ExtensionHostTest, MissingManifestMeansNoExtensions) {
  CreateHost();
  EXPECT_TRUE(host_->LoadFromManifest().ok());
  EXPECT_TRUE(host_->attached_ids().empty());
  EXPECT_TRUE(host_->errors().empty());
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

}  // namespace
}  // namespace momo::engine
