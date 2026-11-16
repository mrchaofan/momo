#include "embedder/macos/mac_platform_integration.h"

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>

#include <optional>

#include "base/logging.h"
#include "embedder/common/engine_platform.h"
#include "embedder/common/fallback_surface.h"
#include "embedder/macos/mac_mouse_events.h"
#import "embedder/macos/mac_menu.h"
#include "embedder/macos/mac_native_loop.h"
#include "embedder/macos/mac_surface_metal.h"
#include "embedder/macos/mac_vsync_displaylink.h"
#include "embedder/macos/mac_window.h"

namespace momo::embedder::macos {

namespace {

class MacServices final : public momo::embedder::PlatformServices {
 public:
  StatusOr<std::string> GetClipboardText() override {
    NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    return std::string(text ? text.UTF8String : "");
  }

  Status SetClipboardText(std::string_view utf8) override {
    NSPasteboard* board = [NSPasteboard generalPasteboard];
    [board clearContents];
    NSString* text = [[NSString alloc] initWithBytes:utf8.data()
                                              length:utf8.size()
                                            encoding:NSUTF8StringEncoding];
    if (text == nil || ![board setString:text forType:NSPasteboardTypeString]) {
      return InternalError("Failed to write clipboard");
    }
    return Status::Ok();
  }

  Status OpenUrl(std::string_view url) override {
    NSString* string = [[NSString alloc] initWithBytes:url.data()
                                                length:url.size()
                                              encoding:NSUTF8StringEncoding];
    NSURL* ns_url = string ? [NSURL URLWithString:string] : nil;
    if (ns_url == nil) {
      return InvalidArgumentError("Invalid URL");
    }
    return [[NSWorkspace sharedWorkspace] openURL:ns_url] ? Status::Ok()
                                                          : InternalError("openURL failed");
  }
};

// 应用级能力: NSApplication 的隐藏/显示/激活。
class MacApp final : public momo::embedder::PlatformApp {
 public:
  Status Hide() override {
    [NSApp hide:nil];  // applicationDidHide → Application::OnAppVisibilityChanged(false)
    return Status::Ok();
  }
  Status Show() override {
    [NSApp unhide:nil];
    return Status::Ok();
  }
  Status Activate(bool steal) override {
    if (steal) {
      [NSApp activateIgnoringOtherApps:YES];
    } else {
      [NSApp requestUserAttention:NSInformationalRequest];  // 不抢占前台: Dock 图标提示
    }
    return Status::Ok();
  }
};

class MacPlatformIntegration final : public PlatformIntegration {
 public:
  MacPlatformIntegration() {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    menus_.emplace();  // 默认的应用菜单
    if (SoftwareRenderingForced()) {
      MOMO_LOG(INFO) << "MOMO_SOFTWARE_RENDERING set: windows use the software surface.";
    } else if (auto device = MacMetalDevice::Create(); device.ok()) {
      metal_device_ = std::move(device).value();
    } else {
      MOMO_LOG(WARNING) << "Metal unavailable (" << device.status() << "); windows use the software surface.";
    }
  }

  StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(const WindowOptions& options,
                                                         WindowDelegate* delegate) override {
    if (delegate == nullptr) {
      return InvalidArgumentError("CreateWindow requires a delegate");
    }
    return std::unique_ptr<PlatformWindow>(std::make_unique<MacWindow>(options, delegate, mouse_input_, metal_device_));
  }

  StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource() override {
    MOMO_ASSIGN_OR_RETURN(ObjectPtr<MacVsyncDisplayLink> link, MacVsyncDisplayLink::Create());
    vsync_ = WeakPtr<MacVsyncDisplayLink>(link.get());  // 所有权在调用方(Shell);这里只为切换显示器
    return ObjectPtr<platform::VsyncSource>(link.release());
  }

  void SetVsyncWindow(const PlatformWindow* window) override {
    if (MacVsyncDisplayLink* link = vsync_.get()) {
      // native_handle() 为内容 NSView;display link 只弱引用其 NSWindow(可能晚于窗口销毁)。
      link->SetWindow(window != nullptr ? ((__bridge NSView*)window->native_handle()).window : nil);
    }
  }

  momo::embedder::PlatformServices& services() override { return services_; }
  momo::embedder::PlatformApp& app() override { return app_; }
  PlatformMenus* menus() override { return &menus_.value(); }

  std::vector<DisplayInfo> displays() const override {
    std::vector<DisplayInfo> out;
    for (NSScreen* screen in [NSScreen screens]) {
      DisplayInfo info;
      NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
      info.id = number.unsignedLongLongValue;
      info.bounds = ScreenRectFromCocoa(screen.frame);
      info.work_area = ScreenRectFromCocoa(screen.visibleFrame);
      info.device_pixel_ratio = screen.backingScaleFactor;
      if (@available(macOS 12.0, *)) {
        if (screen.maximumFramesPerSecond > 0) {
          info.refresh_rate = screen.maximumFramesPerSecond;
        }
      }
      out.push_back(info);
    }
    return out;
  }

  NativeLoop& native_loop() override { return native_loop_; }

 private:
  MacNativeLoop native_loop_;
  MacServices services_;
  MacApp app_;
  std::optional<MacMenus> menus_;  // NSApplication 初始化之后构造
  // 进程内唯一的指针输入(PointerDispatcher), 注入每个窗口;窗口先于本对象销毁。
  MacMouseInput mouse_input_;
  // 应用级的 Metal 设备与命令队列: 全部窗口共用(引擎因此只建一个 GPU 后端)。为空 = 软件呈现。
  std::shared_ptr<const MacMetalDevice> metal_device_;
  // 应用的帧时钟来源(由 Shell 持有;已释放时为空)。
  WeakPtr<MacVsyncDisplayLink> vsync_;
};

}  // namespace

std::unique_ptr<PlatformIntegration> CreateMacPlatformIntegration() {
  return std::make_unique<MacPlatformIntegration>();
}

}  // namespace momo::embedder::macos
