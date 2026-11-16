// 原生扩展的端到端测试: 真实的引擎(Shell + Node 运行时)执行一段 JS, JS import 真实的插件(.node)。
// 放在 extensions/ 而不是 engine/: 引擎的测试不依赖示例代码(依赖方向 examples → engine)。

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "base/event_loop.h"
#include "engine/common/standard_codec.h"
#include "engine/shell/shell.h"
#include "engine/shell/testing/shell_test_utils.h"
#include "engine/shell/thread_host.h"
#include "gtest/gtest.h"
#include "momo/momo_host.h"

namespace momo::engine {
namespace {

using testing::WaitFor;

class NativeExtensionTest : public ::testing::Test {
 protected:
  NativeExtensionTest()
      : platform_loop_(EventLoop::Create()),
        dir_(std::filesystem::temp_directory_path() /
             (std::string("momo_ext_") +
              ::testing::UnitTest::GetInstance()->current_test_info()->name())) {
    platform_loop_->BindToCurrentThread();
    runners_ = threads_.MakeTaskRunners(platform_loop_->GetTaskRunner());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  ~NativeExtensionTest() override {
    if (shell_) shell_->Shutdown();
    shell_.reset();
    platform_loop_->Terminate();
    platform_loop_->RunUntilIdle();
    std::error_code ignored;  // Windows: 已加载的插件文件删不掉
    std::filesystem::remove_all(dir_, ignored);
  }

  // 运行一段 ESM 源码。其中 `report(text)` 把一行文本交给测试(reports_)。
  void Launch(const std::string& source) {
    const std::filesystem::path bundle = dir_ / "main.mjs";
    std::ofstream(bundle) << R"JS(
      import { send } from 'momo:services';
      const report = (text) => send('test/report', new TextEncoder().encode(String(text)));
    )JS" << source;
    Settings settings;
    settings.bundle_path = bundle;
    auto shell = Shell::Create(runners_, settings);
    ASSERT_TRUE(shell.ok()) << shell.status();
    shell_ = std::move(shell).value();
    shell_->hub()->SetNativeHandler("test/report", runners_.platform,
                                    [this](MessageBytes message, MessageReply) {
                                      reports_.emplace_back(message.begin(), message.end());
                                    });
    std::optional<Status> result;
    shell_->Run([&](Status status) { result = std::move(status); });
    ASSERT_TRUE(PumpUntil([&] { return result.has_value(); }));
    ASSERT_TRUE(result->ok()) << *result;
  }

  bool PumpUntil(const std::function<bool()>& condition) {
    return WaitFor(condition, std::chrono::milliseconds(5000), platform_loop_.get());
  }

  // JS 字符串字面量形式的文件路径。
  static std::string Js(const std::filesystem::path& path) {
    std::string out = "'";
    for (char c : path.generic_string()) {
      if (c == '\'' || c == '\\') out += '\\';
      out += c;
    }
    return out + "'";
  }

  std::shared_ptr<EventLoop> platform_loop_;
  ThreadHost threads_{"test"};
  TaskRunners runners_;
  std::filesystem::path dir_;
  std::unique_ptr<Shell> shell_;
  std::vector<std::string> reports_;
};

// 只用 Node-API 的插件: 同步函数、字符串/字节往返、后台线程上的 Promise。
TEST_F(NativeExtensionTest, HelloAddonEndToEnd) {
  Launch("import addon from " + Js(MOMO_EXT_HELLO_ADDON) + R"JS(;
    report([addon.add(2, 40), addon.greet('世界'),
            Array.from(addon.reverseBytes(new Uint8Array([1, 2, 3]))).join(',')].join('|'));
    report(await addon.hash(new TextEncoder().encode('momo')));
    try { addon.add('x'); } catch (error) { report(error.constructor.name); }
  )JS");
  ASSERT_TRUE(PumpUntil([&] { return reports_.size() == 3; }));
  EXPECT_EQ(reports_[0], "42|Hello, 世界!|3,2,1");
  // FNV-1a 64 of "momo".
  uint64_t expected = 1469598103934665603ULL;
  for (char c : std::string("momo")) expected = (expected ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(expected));
  EXPECT_EQ(reports_[1], hex);
  EXPECT_EQ(reports_[2], "TypeError");
}

// 外部纹理: 插件从 nativeHost 拿到宿主表, 在自己的线程上推帧;引擎的纹理表里出现它的像素。
TEST_F(NativeExtensionTest, TextureAddonPushesFramesFromItsOwnThread) {
  Launch("import addon from " + Js(MOMO_EXT_TEXTURE_ADDON) + R"JS(;
    import { nativeHost } from 'momo:services';
    const id = addon.start(nativeHost, 32, 16);
    report(id);
    globalThis.stop = () => { addon.stop(); report('stopped:' + (addon.frames() > 0)); };
    setTimeout(globalThis.stop, 200);
  )JS");
  ASSERT_TRUE(PumpUntil([&] { return !reports_.empty(); }));
  const int64_t id = std::stoll(reports_[0]);
  ASSERT_GT(id, 0);
  ASSERT_TRUE(PumpUntil([&] {
    auto texture = shell_->textures()->Get(id);
    return texture != nullptr && texture->latest() != nullptr;
  }));
  EXPECT_EQ(shell_->textures()->Get(id)->latest()->width(), 32);
  EXPECT_EQ(shell_->textures()->Get(id)->latest()->height(), 16);
  ASSERT_TRUE(PumpUntil([&] { return reports_.size() == 2; }));
  EXPECT_EQ(reports_[1], "stopped:true");
  EXPECT_EQ(shell_->textures()->Get(id), nullptr) << "stop() destroys the texture";
}

// 出帧线程还在跑时直接关闭引擎: 插件的清理函数停掉线程, 不崩溃、不挂起。
TEST_F(NativeExtensionTest, ShutdownStopsARunningTextureAddon) {
  Launch("import addon from " + Js(MOMO_EXT_TEXTURE_ADDON) + R"JS(;
    import { nativeHost } from 'momo:services';
    report(addon.start(nativeHost, 64, 64));
  )JS");
  ASSERT_TRUE(PumpUntil([&] { return !reports_.empty(); }));
  const int64_t id = std::stoll(reports_[0]);
  ASSERT_TRUE(PumpUntil([&] {
    auto texture = shell_->textures()->Get(id);
    return texture != nullptr && texture->latest() != nullptr;
  }));
  shell_->Shutdown();
}

// momo_host.h 写下的关闭顺序: 关闭宿主表 → dispose 平台视图 → 销毁 JS 运行时(插件的清理函数)。
// 所以清理函数执行时视图已全部 dispose, 宿主表已拒绝调用。
TEST_F(NativeExtensionTest, ShutdownOrderSeenByAnAddon) {
  const std::filesystem::path log = dir_ / "lifecycle.log";
  Launch("import addon from " + Js(MOMO_EXT_LIFECYCLE_ADDON) + R"JS(;
    import { nativeHost } from 'momo:services';
    report('attach:' + addon.attach(nativeHost, )JS" + Js(log) + R"JS());
    // momo/platform_views create {id: 7, viewType: 'test.view', params: null}(StandardMethodCodec, 手写)。
    const text = (s) => { const b = new TextEncoder().encode(s); return [7, b.length, ...b]; };
    const call = new Uint8Array([...text('create'), 13, 3,
                                 ...text('id'), 3, 7, 0, 0, 0,
                                 ...text('viewType'), ...text('test.view'),
                                 ...text('params'), 0]);
    const reply = new Uint8Array(await send('momo/platform_views', call));
    report('create:' + reply[0]);
    report('task:' + addon.postTask());
  )JS");
  ASSERT_TRUE(PumpUntil([&] { return reports_.size() == 3; }));
  EXPECT_EQ(reports_[0], "attach:0");
  EXPECT_EQ(reports_[1], "create:0") << "success envelope";
  EXPECT_EQ(reports_[2], "task:0");
  auto lines = [&] {
    std::ifstream in(log);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) out.push_back(line);
    return out;
  };
  ASSERT_TRUE(PumpUntil([&] { return lines().size() == 2; }));
  EXPECT_EQ(lines(), (std::vector<std::string>{"create:7", "task"}));

  shell_->Shutdown();
  platform_loop_->RunUntilIdle();
  EXPECT_EQ(lines(), (std::vector<std::string>{"create:7", "task", "dispose:7",
                                                "cleanup:" + std::to_string(kMomoErrClosed)}));
}

}  // namespace
}  // namespace momo::engine
