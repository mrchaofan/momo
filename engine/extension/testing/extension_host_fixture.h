#pragma once

// ExtensionHost 测试夹具(引擎与示例扩展的测试共用): 测试线程上的事件循环充当全部 runner,
// 扩展目录为临时目录(写 manifest、拷贝扩展库), 可选安装 JS 运行时(ext:* 模块)。

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "base/testing/scoped_test_event_loop.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/platform_view_registry.h"
#include "engine/extension/extension_host.h"
#include "engine/graphics/texture.h"
#include "engine/runtime/js_runtime.h"
#include "engine/ui/ui_bindings.h"
#include "gtest/gtest.h"

namespace momo::engine::testing {

class FakeClient final : public ui::WindowClient {
 public:
  void ScheduleFrame() override {}
  void Render(std::unique_ptr<graphics::LayerTree>) override {}
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

}  // namespace momo::engine::testing
