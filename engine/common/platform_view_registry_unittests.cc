#include "engine/common/platform_view_registry.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace momo::engine {
namespace {

struct Log {
  std::vector<std::string> events;
};

PlatformViewRegistry::Factory MakeFactory(Log& log, int* native) {
  PlatformViewRegistry::Factory f;
  f.create = [&log, native](int64_t id, const std::vector<uint8_t>& params) -> void* {
    log.events.push_back("create:" + std::to_string(id) + ":" +
                         std::string(params.begin(), params.end()));
    return native;
  };
  f.dispose = [&log](int64_t id, void*) { log.events.push_back("dispose:" + std::to_string(id)); };
  return f;
}

TEST(PlatformViewRegistryTest, CreateGetDispose) {
  Log log;
  int native = 0;
  PlatformViewRegistry registry;
  ASSERT_TRUE(registry.RegisterFactory("acme.view", MakeFactory(log, &native)).ok());
  auto created = registry.Create(1, "acme.view", {'h', 'i'});
  ASSERT_TRUE(created.ok()) << created.status();
  EXPECT_EQ(*created, &native);
  EXPECT_EQ(registry.Get(1), &native);
  EXPECT_EQ(registry.Get(2), nullptr);
  ASSERT_EQ(registry.views().size(), 1u);
  EXPECT_EQ(registry.views()[0].type, "acme.view");

  std::vector<std::string> order;
  ASSERT_TRUE(registry.Dispose(1, [&](int64_t id) {
    log.events.push_back("before:" + std::to_string(id));
  }).ok());
  EXPECT_EQ(log.events, (std::vector<std::string>{"create:1:hi", "before:1", "dispose:1"}));
  EXPECT_EQ(registry.Get(1), nullptr);
  EXPECT_FALSE(registry.Dispose(1).ok());
}

TEST(PlatformViewRegistryTest, RejectsBadInput) {
  Log log;
  int native = 0;
  PlatformViewRegistry registry;
  EXPECT_FALSE(registry.RegisterFactory("", MakeFactory(log, &native)).ok());
  EXPECT_FALSE(registry.RegisterFactory("momo/web", MakeFactory(log, &native)).ok())
      << "momo/ is reserved";
  ASSERT_TRUE(registry.RegisterFactory("a.view", MakeFactory(log, &native)).ok());
  EXPECT_FALSE(registry.RegisterFactory("a.view", MakeFactory(log, &native)).ok());
  EXPECT_FALSE(registry.Create(1, "unknown.view", {}).ok());
  EXPECT_FALSE(registry.Create(0, "a.view", {}).ok()) << "ids must be positive";
  ASSERT_TRUE(registry.Create(1, "a.view", {}).ok());
  EXPECT_FALSE(registry.Create(1, "a.view", {}).ok()) << "duplicate id";

  // 工厂返回空 = 创建失败, 不入表。
  PlatformViewRegistry::Factory failing;
  failing.create = [](int64_t, const std::vector<uint8_t>&) -> void* { return nullptr; };
  failing.dispose = [](int64_t, void*) {};
  ASSERT_TRUE(registry.RegisterFactory("b.view", std::move(failing)).ok());
  EXPECT_FALSE(registry.Create(2, "b.view", {}).ok());
  EXPECT_EQ(registry.size(), 1u);
}

// 引擎关闭: 全部视图被销毁, 工厂也清空——之后不会再调用任何工厂回调。
TEST(PlatformViewRegistryTest, ClearDisposesViewsAndForgetsFactories) {
  Log log;
  int native = 0;
  PlatformViewRegistry registry;
  ASSERT_TRUE(registry.RegisterFactory("a.view", MakeFactory(log, &native)).ok());
  ASSERT_TRUE(registry.Create(1, "a.view", {}).ok());
  ASSERT_TRUE(registry.Create(3, "a.view", {}).ok());
  log.events.clear();

  int before = 0;
  registry.Clear([&](int64_t) { before++; });
  EXPECT_EQ(before, 2);
  EXPECT_EQ(log.events, (std::vector<std::string>{"dispose:1", "dispose:3"}));
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_FALSE(registry.Create(4, "a.view", {}).ok()) << "the factory is gone";
  EXPECT_EQ(log.events.size(), 2u);
}

}  // namespace
}  // namespace momo::engine
