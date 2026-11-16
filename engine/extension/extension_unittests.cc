// 宿主能力表(momo_host.h)的单元测试: 只经 C 函数表调用, 与原生扩展看到的一样。
// 真实插件(.node)经引擎加载的端到端测试在 extensions/examples/examples_unittests.cc。

#include "engine/extension/extension_host.h"

#include <thread>
#include <vector>

#include "base/testing/scoped_test_event_loop.h"
#include "gtest/gtest.h"

namespace momo::engine {
namespace {

class ExtensionHostTest : public ::testing::Test {
 protected:
  // 测试线程上的事件循环充当 platform 线程。
  momo::testing::ScopedTestEventLoop loop_;
  std::shared_ptr<graphics::TextureRegistry> textures_ = std::make_shared<graphics::TextureRegistry>();
  std::shared_ptr<PlatformViewRegistry> views_ = std::make_shared<PlatformViewRegistry>();
  ExtensionHost extension_host_{loop_.task_runner(), textures_, views_};
  const MomoHost* const host_ = extension_host_.host();
};

TEST_F(ExtensionHostTest, TableIsVersioned) {
  EXPECT_EQ(host_->struct_size, sizeof(MomoHost));
  EXPECT_EQ(host_->api_version, MOMO_HOST_API_VERSION);
}

TEST_F(ExtensionHostTest, PlatformTasksRunOnThePlatformThreadInOrder) {
  struct Record {
    std::vector<int> order;
    std::thread::id thread;
  } record;
  struct Item {
    Record* record;
    int value;
  } first{&record, 1}, second{&record, 2};
  const MomoTaskFn run = [](void* data) {
    auto* item = static_cast<Item*>(data);
    item->record->order.push_back(item->value);
    item->record->thread = std::this_thread::get_id();
  };
  // 从别的线程投递, 在 platform 线程执行。
  std::thread([&] {
    EXPECT_EQ(host_->post_platform_task(host_, run, &first), kMomoOk);
    EXPECT_EQ(host_->post_platform_task(host_, run, &second), kMomoOk);
  }).join();
  EXPECT_TRUE(record.order.empty());
  loop_.RunUntilIdle();
  EXPECT_EQ(record.order, (std::vector<int>{1, 2}));
  EXPECT_EQ(record.thread, std::this_thread::get_id());
  EXPECT_EQ(host_->post_platform_task(host_, nullptr, nullptr), kMomoErrInvalidArgs);
}

TEST_F(ExtensionHostTest, TexturesReceivePixelsAndNotifyTheEngine) {
  std::vector<int64_t> frames;
  textures_->SetFrameListener(loop_.task_runner(), [&](int64_t id) { frames.push_back(id); });
  int64_t id = 0;
  ASSERT_EQ(host_->texture_create(host_, &id), kMomoOk);
  ASSERT_NE(textures_->Get(id), nullptr);
  EXPECT_EQ(textures_->Get(id)->latest(), nullptr);

  const std::vector<uint8_t> pixels(2 * 2 * 4, 255);
  EXPECT_EQ(host_->texture_update_pixels(host_, id, pixels.data(), 2, 2, 8), kMomoOk);
  loop_.RunUntilIdle();
  EXPECT_NE(textures_->Get(id)->latest(), nullptr);
  EXPECT_EQ(frames, (std::vector<int64_t>{id})) << "a new frame asks the engine to draw";

  EXPECT_EQ(host_->texture_update_pixels(host_, id, nullptr, 2, 2, 8), kMomoErrInvalidArgs);
  EXPECT_EQ(host_->texture_update_pixels(host_, id + 1, pixels.data(), 2, 2, 8), kMomoErrNotFound);
  EXPECT_EQ(host_->texture_destroy(host_, id), kMomoOk);
  EXPECT_EQ(textures_->Get(id), nullptr);
  EXPECT_EQ(host_->texture_destroy(host_, id), kMomoErrNotFound);
  textures_->SetFrameListener(nullptr, nullptr);
}

struct ViewLog {
  std::vector<std::string> events;
  int native = 0;
};

MomoPlatformViewFactory MakeFactory(ViewLog* log) {
  MomoPlatformViewFactory factory{};
  factory.struct_size = sizeof(factory);
  factory.user_data = log;
  factory.create = [](void* data, int64_t id, const uint8_t* params, size_t len) -> void* {
    auto* log = static_cast<ViewLog*>(data);
    log->events.push_back("create:" + std::to_string(id) + ":" + std::string(params, params + len));
    return &log->native;
  };
  factory.dispose = [](void* data, int64_t id, void*) {
    static_cast<ViewLog*>(data)->events.push_back("dispose:" + std::to_string(id));
  };
  return factory;
}

TEST_F(ExtensionHostTest, PlatformViewFactoriesServeTheRegistry) {
  ViewLog log;
  const MomoPlatformViewFactory factory = MakeFactory(&log);
  // 注册可以在任意线程(插件在 JS 线程上注册)。
  std::thread([&] {
    EXPECT_EQ(host_->platform_view_register_factory(host_, "acme.view", &factory), kMomoOk);
    EXPECT_EQ(host_->platform_view_register_factory(host_, "acme.view", &factory), kMomoErrInvalidArgs);
    EXPECT_EQ(host_->platform_view_register_factory(host_, "momo/view", &factory), kMomoErrInvalidArgs);
    void* native = nullptr;
    EXPECT_EQ(host_->platform_view_get(host_, 1, &native), kMomoErrWrongThread);
  }).join();

  // 引擎(Shell)按 JS 的请求创建/销毁视图。
  ASSERT_TRUE(views_->Create(1, "acme.view", {'h', 'i'}).ok());
  void* native = nullptr;
  EXPECT_EQ(host_->platform_view_get(host_, 1, &native), kMomoOk);
  EXPECT_EQ(native, &log.native);
  ASSERT_TRUE(views_->Dispose(1).ok());
  EXPECT_EQ(host_->platform_view_get(host_, 1, &native), kMomoErrNotFound);
  EXPECT_EQ(log.events, (std::vector<std::string>{"create:1:hi", "dispose:1"}));
}

// 关闭之后: 每个函数都被拒绝, 已投递而未执行的平台任务不再执行。
TEST_F(ExtensionHostTest, ClosedTableRejectsEverythingAndDropsQueuedTasks) {
  int runs = 0;
  ASSERT_EQ(host_->post_platform_task(host_, [](void* data) { ++*static_cast<int*>(data); }, &runs),
            kMomoOk);
  int64_t id = 0;
  ASSERT_EQ(host_->texture_create(host_, &id), kMomoOk);

  extension_host_.Close();
  loop_.RunUntilIdle();
  EXPECT_EQ(runs, 0) << "a task queued before Close must not run after it";

  ViewLog log;
  const MomoPlatformViewFactory factory = MakeFactory(&log);
  const uint8_t pixel[4] = {0, 0, 0, 255};
  void* native = nullptr;
  EXPECT_EQ(host_->post_platform_task(host_, [](void*) {}, nullptr), kMomoErrClosed);
  EXPECT_EQ(host_->texture_create(host_, &id), kMomoErrClosed);
  EXPECT_EQ(host_->texture_update_pixels(host_, id, pixel, 1, 1, 4), kMomoErrClosed);
  EXPECT_EQ(host_->texture_destroy(host_, id), kMomoErrClosed);
  EXPECT_EQ(host_->platform_view_register_factory(host_, "acme.view", &factory), kMomoErrClosed);
  EXPECT_EQ(host_->platform_view_get(host_, 1, &native), kMomoErrClosed);
}

}  // namespace
}  // namespace momo::engine
