#include "embedder/macos/mac_platform_integration.h"

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>

#include "embedder/common/engine_platform.h"
#include "embedder/macos/mac_mouse_events.h"
#include "embedder/macos/mac_native_loop.h"
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
  uint32_t capabilities() const override { return kAppCapabilityHide | kAppCapabilityActivate; }
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
  }

  StatusOr<std::unique_ptr<PlatformWindow>> CreateWindow(const WindowSpec& spec,
                                                         WindowDelegate* delegate) override {
    if (delegate == nullptr) {
      return InvalidArgumentError("CreateWindow requires a delegate");
    }
    return std::unique_ptr<PlatformWindow>(std::make_unique<MacWindow>(spec, delegate, mouse_input_));
  }

  StatusOr<ObjectPtr<platform::VsyncSource>> CreateVsyncSource(const PlatformWindow& window) override {
    // native_handle() 为内容 NSView;platform::VsyncSource 只弱引用其 NSWindow(可能晚于窗口销毁)。
    NSView* view = (__bridge NSView*)window.native_handle();
    return MacVsyncDisplayLink::Create(view.window);
  }

  momo::embedder::PlatformServices& services() override { return services_; }
  momo::embedder::PlatformApp& app() override { return app_; }

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
  // 进程内唯一的指针输入(PointerDispatcher), 注入每个窗口;窗口先于本对象销毁。
  MacMouseInput mouse_input_;
};

}  // namespace

std::unique_ptr<PlatformIntegration> CreateMacPlatformIntegration() {
  return std::make_unique<MacPlatformIntegration>();
}

}  // namespace momo::embedder::macos
