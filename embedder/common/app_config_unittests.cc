#include "embedder/common/app_config.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace momo::embedder {
namespace {

StatusOr<AppConfig> Parse(std::vector<std::string> args) { return AppConfig::Parse(args); }

TEST(AppConfigTest, Defaults) {
  auto config = Parse({});
  ASSERT_TRUE(config.ok());
  EXPECT_FALSE(config->bundle_path.has_value());
  EXPECT_FALSE(config->assets_dir.has_value());
  EXPECT_FALSE(config->dev_mode);
  EXPECT_EQ(config->window.title, "Momo");
  EXPECT_EQ(config->window.size, (Size{1024, 768}));
}

TEST(AppConfigTest, ParsesAllFlags) {
  auto config = Parse({"--bundle=/tmp/app/bundle.js", "--assets=/tmp/app/assets", "--dev",
                       "--title=你好 Momo", "--width=800", "--height=600"});
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->bundle_path->string(), "/tmp/app/bundle.js");
  EXPECT_EQ(config->assets_dir->string(), "/tmp/app/assets");
  EXPECT_TRUE(config->dev_mode);
  EXPECT_EQ(config->window.title, "你好 Momo");
  EXPECT_EQ(config->window.size, (Size{800, 600}));
}

TEST(AppConfigTest, IgnoresSystemAndUnknownArgs) {
  auto config = Parse({"-psn_0_12345", "-NSDocumentRevisionsDebugMode", "YES", "--unknown=1"});
  ASSERT_TRUE(config.ok());
  EXPECT_FALSE(config->dev_mode);
}

TEST(AppConfigTest, RejectsBadValues) {
  EXPECT_EQ(Parse({"--width=abc"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--width=0"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--height=12px"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--bundle"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--bundle="}).status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace momo::embedder
