#include "engine/common/app_layout.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace momo::engine {
namespace {

class AppLayoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("momo_app_layout_" +
             std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  void WritePackage(const std::string& json) { std::ofstream(root_ / "package.json") << json; }

  std::filesystem::path root_;
};

TEST_F(AppLayoutTest, DirectoryUsesPackageMain) {
  WritePackage(R"({"name": "demo", "main": "dist/main.js"})");
  auto app = LocateApp(root_);
  ASSERT_TRUE(app.ok()) << app.status();
  EXPECT_EQ(app->entry, root_ / "dist" / "main.js") << "the entry is located, not checked for existence";
  EXPECT_EQ(app->assets_dir, root_ / "assets");
}

TEST_F(AppLayoutTest, DirectoryWithoutMainDefaultsToIndexJs) {
  WritePackage(R"({"name": "demo"})");
  auto app = LocateApp(root_);
  ASSERT_TRUE(app.ok()) << app.status();
  EXPECT_EQ(app->entry, root_ / "index.js");
}

TEST_F(AppLayoutTest, FileIsItsOwnEntry) {
  auto app = LocateApp(root_ / "tools" / "main.js");
  ASSERT_TRUE(app.ok()) << app.status();
  EXPECT_EQ(app->entry, root_ / "tools" / "main.js") << "a missing file fails when it is loaded";
  EXPECT_EQ(app->assets_dir, root_ / "tools" / "assets");
}

TEST_F(AppLayoutTest, RelativePathsBecomeAbsolute) {
  auto app = LocateApp("some/app.js");
  ASSERT_TRUE(app.ok());
  EXPECT_TRUE(app->entry.is_absolute());
  EXPECT_TRUE(app->assets_dir.is_absolute());
}

TEST_F(AppLayoutTest, DirectoryNeedsAUsablePackageJson) {
  EXPECT_EQ(LocateApp(root_).status().code(), StatusCode::kNotFound) << "no package.json";
  WritePackage("{ not json");
  EXPECT_EQ(LocateApp(root_).status().code(), StatusCode::kInvalidArgument);
  WritePackage(R"({"main": 42})");
  EXPECT_EQ(LocateApp(root_).status().code(), StatusCode::kInvalidArgument);
  WritePackage(R"({"main": ""})");
  EXPECT_EQ(LocateApp(root_).status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace momo::engine
