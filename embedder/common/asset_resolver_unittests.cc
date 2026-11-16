#include "embedder/common/asset_resolver.h"

#include <filesystem>
#include <fstream>

#include "gtest/gtest.h"

namespace momo::embedder {
namespace {

class AssetResolverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("momo_assets_test_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::create_directories(root_ / "Resources");
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  void Touch(const std::filesystem::path& p) { std::ofstream(p) << "x"; }

  std::filesystem::path root_;
};

TEST_F(AssetResolverTest, DefaultsToResourcesLayout) {
  Touch(root_ / "Resources" / "bundle.js");
  AssetResolver resolver(root_ / "Resources");
  auto assets = resolver.Resolve({});
  ASSERT_TRUE(assets.ok()) << assets.status();
  EXPECT_EQ(assets->bundle_path, (root_ / "Resources" / "bundle.js").lexically_normal());
  EXPECT_EQ(assets->assets_dir, (root_ / "Resources" / "assets").lexically_normal());
  EXPECT_TRUE(assets->bundle_path.is_absolute());
}

// bundle 只定位、不预先检查: 不存在(或不是文件)要到加载时才失败。
TEST_F(AssetResolverTest, BundleIsLocatedButNotChecked) {
  auto packaged = AssetResolver(root_ / "Resources").Resolve({});
  ASSERT_TRUE(packaged.ok()) << packaged.status();
  EXPECT_EQ(packaged->bundle_path, (root_ / "Resources" / "bundle.js").lexically_normal());
  AppConfig config;
  config.bundle_path = root_ / "nope.js";
  auto given = AssetResolver(root_ / "Resources").Resolve(config);
  ASSERT_TRUE(given.ok()) << given.status();
  EXPECT_EQ(given->bundle_path.filename(), "nope.js");
}

TEST_F(AssetResolverTest, ExplicitPathsOverrideDefaults) {
  Touch(root_ / "dev.js");
  std::filesystem::create_directories(root_ / "dev_assets");
  AppConfig config;
  config.bundle_path = root_ / "dev.js";
  config.assets_dir = root_ / "dev_assets";
  auto assets = AssetResolver(root_ / "Resources").Resolve(config);
  ASSERT_TRUE(assets.ok()) << assets.status();
  EXPECT_EQ(assets->bundle_path.filename(), "dev.js");
  EXPECT_EQ(assets->assets_dir.filename(), "dev_assets");
}

TEST_F(AssetResolverTest, ExplicitMissingAssetsDirIsNotFound) {
  Touch(root_ / "Resources" / "bundle.js");
  AppConfig config;
  config.assets_dir = root_ / "nope";
  EXPECT_EQ(AssetResolver(root_ / "Resources").Resolve(config).status().code(),
            StatusCode::kNotFound);
}

}  // namespace
}  // namespace momo::embedder
