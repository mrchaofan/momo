#pragma once

// HostChannel: 宿主在引擎消息通道上注册的一个方法通道(StandardMethodCodec)。所有宿主通道(momo/app、
// momo/window…)共用: 方法以注册表分发(新增接口 = Register 一行), 宿主 → JS 的通知与询问也在这里。
//
// 错误回复的 code: "unimplemented"(平台不支持该能力)、"notFound"、"invalidArgument"、"error"。
// 线程约定: 在 platform 线程创建、调用与析构;处理器在 platform 线程运行。

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/closure.h"
#include "base/geometry.h"
#include "base/macros.h"
#include "base/status.h"
#include "base/task_runner.h"
#include "engine/common/message_channel_hub.h"
#include "engine/common/standard_codec.h"

namespace momo::embedder {

class HostChannel final {
 public:
  using Handler = std::function<StatusOr<engine::EncodableValue>(const engine::EncodableValue& args)>;

  HostChannel(std::shared_ptr<engine::MessageChannelHub> hub, std::shared_ptr<TaskRunner> platform_runner,
              std::string name);
  ~HostChannel();  // 移除处理器

  const std::string& name() const { return name_; }

  // JS → 宿主: 注册方法(同名覆盖)。
  void Register(std::string method, Handler handler);
  // 分发一次调用(未知方法 → Unimplemented)。测试可直接调用。
  StatusOr<engine::EncodableValue> Dispatch(const engine::MethodCall& call) const;

  // 宿主 → JS: 通知(不等回复)。
  void Notify(std::string method, engine::EncodableValue args = {});
  // 宿主 → JS: 询问, 回复 bool。reply 在 platform 线程回调;JS 无处理器、出错或运行时已不在时为 true(允许)。
  // 回复不依赖本对象存活。
  void Ask(std::string method, engine::EncodableValue args, MoveOnlyFunction<void(bool allow)> reply);

 private:
  using Handlers = std::map<std::string, Handler, std::less<>>;

  const std::shared_ptr<engine::MessageChannelHub> hub_;
  const std::shared_ptr<TaskRunner> platform_runner_;
  const std::string name_;
  // 与 hub 上的处理器共享: 本对象析构后, 已排队的调用看到空表(回复 Unavailable)。
  std::shared_ptr<Handlers> handlers_;

  MOMO_DISALLOW_COPY_AND_ASSIGN(HostChannel);
};

// ---- 参数读取(缺失或类型不符时取 fallback) ----
namespace channel_args {

double Num(const engine::EncodableValue& args, std::string_view key, double fallback);
std::optional<double> OptNum(const engine::EncodableValue& args, std::string_view key);
bool Bool(const engine::EncodableValue& args, std::string_view key, bool fallback);
std::string Str(const engine::EncodableValue& args, std::string_view key);
std::optional<int64_t> Int(const engine::EncodableValue& args, std::string_view key);
engine::EncodableValue RectValue(const Rect& r);
engine::EncodableValue OptRectValue(const std::optional<Rect>& r);

}  // namespace channel_args

}  // namespace momo::embedder
