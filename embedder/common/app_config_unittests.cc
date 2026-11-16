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
  EXPECT_FALSE(config->app_path.has_value());
  EXPECT_FALSE(config->dev_mode);
  EXPECT_EQ(config->window.title, "Momo");
  EXPECT_EQ(config->window.size, (Size{1024, 768}));
  EXPECT_TRUE(config->startup_window);
}

TEST(AppConfigTest, ParsesAllFlags) {
  auto config = Parse({"--dev", "/tmp/app", "--title=你好 Momo", "--width=800", "--height=600",
                       "--no-startup-window", "ignored/second/path"});
  ASSERT_TRUE(config.ok()) << config.status();
  EXPECT_EQ(config->app_path->string(), "/tmp/app") << "the first positional argument is the app";
  EXPECT_TRUE(config->dev_mode);
  EXPECT_EQ(config->window.title, "你好 Momo");
  EXPECT_EQ(config->window.size, (Size{800, 600}));
  EXPECT_FALSE(config->startup_window);
}

TEST(AppConfigTest, IgnoresSystemAndUnknownArgs) {
  auto config = Parse({"-psn_0_12345", "-NSDocumentRevisionsDebugMode", "YES", "--unknown=1"});
  ASSERT_TRUE(config.ok());
  EXPECT_FALSE(config->dev_mode);
  EXPECT_FALSE(config->app_path.has_value()) << "YES is the value of a system argument, not an app";

  // 系统参数的值之后, 位置参数照常是应用路径。
  config = Parse({"-AppleLanguages", "(en)", "examples/counter"});
  ASSERT_TRUE(config.ok());
  EXPECT_EQ(config->app_path->string(), "examples/counter");
}

TEST(AppConfigTest, RejectsBadValues) {
  EXPECT_EQ(Parse({"--width=abc"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--width=0"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--height=12px"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--title"}).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(Parse({"--title="}).status().code(), StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace momo::embedder
