#include "engine/common/platform_view_registry.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace momo::engine {
namespace {

struct Log {
  std::vector<std::string> events;
};

PlatformViewRegistry::Factory MakeFactory(Log& log, const std::string& owner, int* native) {
  PlatformViewRegistry::Factory f;
  f.owner = owner;
  f.create = [&log, native](int64_t id, const std::vector<uint8_t>& params, std::string*) -> void* {
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
  ASSERT_TRUE(registry.RegisterFactory("acme.view", MakeFactory(log, "acme", &native)).ok());
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
  EXPECT_FALSE(registry.RegisterFactory("", MakeFactory(log, "a", &native)).ok());
  EXPECT_FALSE(registry.RegisterFactory("momo/web", MakeFactory(log, "a", &native)).ok())
      << "momo/ is reserved";
  ASSERT_TRUE(registry.RegisterFactory("a.view", MakeFactory(log, "a", &native)).ok());
  EXPECT_FALSE(registry.RegisterFactory("a.view", MakeFactory(log, "a", &native)).ok());
  EXPECT_FALSE(registry.Create(1, "unknown.view", {}).ok());
  EXPECT_FALSE(registry.Create(0, "a.view", {}).ok()) << "ids must be positive";
  ASSERT_TRUE(registry.Create(1, "a.view", {}).ok());
  EXPECT_FALSE(registry.Create(1, "a.view", {}).ok()) << "duplicate id";

  // 工厂返回空 = 创建失败, 不入表。
  PlatformViewRegistry::Factory failing;
  failing.owner = "b";
  failing.create = [](int64_t, const std::vector<uint8_t>&, std::string* error) -> void* {
    *error = "boom";
    return nullptr;
  };
  failing.dispose = [](int64_t, void*) {};
  ASSERT_TRUE(registry.RegisterFactory("b.view", std::move(failing)).ok());
  auto result = registry.Create(2, "b.view", {});
  ASSERT_FALSE(result.ok());
  EXPECT_NE(std::string(result.status().message()).find("boom"), std::string::npos);
  EXPECT_EQ(registry.size(), 1u);
}

TEST(PlatformViewRegistryTest, DisposeAllAndByOwner) {
  Log log;
  int native = 0;
  PlatformViewRegistry registry;
  ASSERT_TRUE(registry.RegisterFactory("a.view", MakeFactory(log, "a", &native)).ok());
  ASSERT_TRUE(registry.RegisterFactory("b.view", MakeFactory(log, "b", &native)).ok());
  ASSERT_TRUE(registry.Create(1, "a.view", {}).ok());
  ASSERT_TRUE(registry.Create(2, "b.view", {}).ok());
  ASSERT_TRUE(registry.Create(3, "a.view", {}).ok());
  log.events.clear();

  // 注销工厂后已创建的视图仍可被正确销毁。
  registry.UnregisterFactoriesOf("a");
  EXPECT_FALSE(registry.HasFactory("a.view"));
  EXPECT_TRUE(registry.HasFactory("b.view"));
  registry.DisposeViewsOf("a");
  EXPECT_EQ(log.events, (std::vector<std::string>{"dispose:1", "dispose:3"}));
  EXPECT_EQ(registry.size(), 1u);

  log.events.clear();
  int before = 0;
  registry.DisposeAll([&](int64_t) { before++; });
  EXPECT_EQ(before, 1);
  EXPECT_EQ(log.events, (std::vector<std::string>{"dispose:2"}));
  EXPECT_EQ(registry.size(), 0u);
}

}  // namespace
}  // namespace momo::engine
