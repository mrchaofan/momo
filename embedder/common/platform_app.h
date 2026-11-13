#pragma once

// PlatformApp: 应用级的平台能力(docs/03 §2)。embedder 的平台抽象, engine 不可见;由宿主的 momo/app 通道
// (app_channel.h)调用。新增应用能力 = 在这里加方法与能力位、各平台实现、在 AppChannel 注册一个方法,
// 再在框架的 app 对象上加方法。
// 线程约定: 仅 platform 线程调用。

#include <cstdint>
#include <string>
#include <string_view>

#include "base/status.h"

namespace momo::embedder {

enum AppCapability : uint32_t {
  kAppCapabilityHide = 1u << 0,      // Hide / Show(整个应用, 如 macOS Cmd+H)
  kAppCapabilityActivate = 1u << 1,  // Activate: 把应用带到前台
};

class PlatformApp {
 public:
  virtual ~PlatformApp() = default;
  virtual uint32_t capabilities() const { return 0; }
  // 隐藏/显示整个应用(全部窗口)。隐藏后宿主进入 Suspended。
  virtual Status Hide() { return Unsupported("hide"); }
  virtual Status Show() { return Unsupported("show"); }
  // 把应用带到前台。steal: 即使用户正在使用别的应用也抢占前台(否则只请求关注)。
  virtual Status Activate(bool steal) {
    (void)steal;
    return Unsupported("focus");
  }

 protected:
  static Status Unsupported(std::string_view what) {
    return UnimplementedError(std::string(what) + " is not supported by this platform");
  }
};

}  // namespace momo::embedder
