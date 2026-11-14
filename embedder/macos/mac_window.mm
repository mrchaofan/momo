#include "embedder/macos/mac_window.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "base/event_loop.h"
#include "base/logging.h"
#include "embedder/macos/mac_key_mapping.h"
#include "embedder/macos/mac_mouse_events.h"
#include "embedder/macos/mac_platform_view_input.h"
#include "embedder/macos/mac_platform_views.h"
#include "embedder/macos/mac_surface_metal.h"
#include "embedder/macos/mac_surface_software.h"
#include "embedder/macos/mac_text_input.h"

using momo::engine::platform::KeyData;
using momo::engine::platform::KeyEventType;
using momo::engine::platform::PointerPhase;
using momo::embedder::macos::FlipScreenY;
using momo::embedder::macos::MacMetalSurface;
using momo::embedder::macos::MacTextInput;
using momo::embedder::macos::MacWindow;

namespace {

NSString* ToNSString(std::string_view utf8) {
  return [[NSString alloc] initWithBytes:utf8.data()
                                  length:utf8.size()
                                encoding:NSUTF8StringEncoding];
}

NSColor* ToNSColor(uint32_t argb) {
  return [NSColor colorWithSRGBRed:((argb >> 16) & 0xFF) / 255.0
                             green:((argb >> 8) & 0xFF) / 255.0
                              blue:(argb & 0xFF) / 255.0
                             alpha:((argb >> 24) & 0xFF) / 255.0];
}

bool SoftwareRenderingForced() {
  const char* value = std::getenv("MOMO_SOFTWARE_RENDERING");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

// 在 theme frame 上命中测试(隐藏标题栏后, 透明标题栏视图与红绿灯浮在内容之上)。
NSView* FrameHit(NSView* content, NSEvent* event) {
  NSView* frame = content.superview;
  return frame != nil ? [frame hitTest:[frame convertPoint:event.locationInWindow fromView:nil]] : nil;
}

// 命中窗口控件按钮(红绿灯)。
bool HitsWindowControl(NSView* hit) {
  for (NSView* v = hit; v != nil; v = v.superview) {
    if ([v isKindOfClass:[NSButton class]]) return true;
  }
  return false;
}

// 双击标题栏(拖动区域)的系统动作: 遵循"连按窗口标题栏以…"设置(缩放 / 最小化 / 无操作)。
void TitleBarDoubleClick(NSWindow* window) {
  NSString* action = [[NSUserDefaults standardUserDefaults] stringForKey:@"AppleActionOnDoubleClick"];
  if ([action isEqualToString:@"Minimize"]) {
    [window miniaturize:nil];
  } else if (![action isEqualToString:@"None"]) {
    [window zoom:nil];
  }
}

// 窗口装饰(主题框架、标题栏、红绿灯等内容视图之外的视图)的跟踪区: 不论事件由谁消费都照常收到移动。
// 它们维护窗口级状态而不是消费事件——典型是边缘缩放光标: 主题框架靠移动得知指针离开了缩放区,
// 才把光标交还给应用;收不到移动时窗口服务器会一直显示缩放光标并忽略应用设置的光标。
// 只转给装饰自己的跟踪区, 不经 AppKit 的全局投递, 所以平台视图(网页)收不到不属于它的移动。
// 返回装饰是否在处理中改变了当前光标(即装饰此刻拥有光标, 如位于缩放区)。
bool ForwardMoveToWindowChrome(NSView* content, NSEvent* event) {
  NSView* frame = content.superview;
  if (frame == nil) return false;
  NSCursor* before = NSCursor.currentCursor;
  MomoDeliverMouseMoved(frame, event, content);  // 内容子树归派发器
  return NSCursor.currentCursor != before;
}

}  // namespace

// ---------------------------------------------------------------------------
// MomoContentView: 键盘 + NSTextInputClient + 光标 + 几何。鼠标事件不经它(窗口的 -sendEvent: 直接交给派发器)。

@interface MomoContentView : NSView <NSTextInputClient>
// 事件位于系统的边缘缩放区(光标归系统、按下先交给 AppKit)。
- (BOOL)eventInSystemResizeZone:(NSEvent*)event;
- (void)attachOwner:(MacWindow*)owner;
- (void)detachOwner;
@property(nonatomic, readonly) MacWindow* owner;  // 已脱离窗口时为 nullptr
- (void)setMomoCursor:(NSCursor*)cursor hidden:(BOOL)hidden;
- (void)refreshCursor;
// screen(主屏左上原点、y 向下的逻辑点)→ 本视图坐标(点, 左下原点)。
- (NSPoint)viewPointFromScreen:(NSPoint)screen;
@end

@implementation MomoContentView {
  MacWindow* _owner;             // 非拥有;MacWindow 析构时清空
  NSTrackingArea* _trackingArea;
  id _keyUpMonitor;
  NSEvent* __weak _lastKeyUp;    // 最近处理过的 keyUp(去重: 监视器补发后 AppKit 若也投递, 只处理一次)
  NSCursor* _cursor;
  BOOL _wantCursorHidden;
  BOOL _cursorHideApplied;
}

- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _cursor = [NSCursor arrowCursor];
    const NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited |
                                          NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                          NSTrackingActiveAlways | NSTrackingInVisibleRect |
                                          NSTrackingEnabledDuringMouseDrag;
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                 options:options
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
  }
  return self;
}

- (void)attachOwner:(MacWindow*)owner {
  _owner = owner;
  // AppKit 在 Command 按下时, 由 -[NSApplication sendEvent:] 吞掉 keyUp, 不发给窗口与 first responder
  // (长期存在的已知行为)。用本地监视器补发, 与 Flutter macOS embedder 的做法相同;
  // 子类化 NSApplication 也能解决, 但要改 NSPrincipalClass, 更侵入。重复投递由 keyUp: 去重。
  MomoContentView* __weak weakSelf = self;
  _keyUpMonitor = [NSEvent
      addLocalMonitorForEventsMatchingMask:NSEventMaskKeyUp
                                   handler:^NSEvent*(NSEvent* event) {
                                     MomoContentView* view = weakSelf;
                                     if (view != nil && event.window == view.window &&
                                         view.window.firstResponder == view &&
                                         (event.modifierFlags & NSEventModifierFlagCommand)) {
                                       [view keyUp:event];
                                     }
                                     return event;
                                   }];
}

- (MacWindow*)owner {
  return _owner;
}

- (void)detachOwner {
  _owner = nullptr;
  if (_keyUpMonitor != nil) {
    [NSEvent removeMonitor:_keyUpMonitor];
    _keyUpMonitor = nil;
  }
  if (_cursorHideApplied) {
    [NSCursor unhide];
    _cursorHideApplied = NO;
  }
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

// 焦点离开内容视图: 若去了某个平台视图, 通知引擎(框架据此让出 momo 焦点, 收起输入法连接)。
- (BOOL)resignFirstResponder {
  const BOOL resigned = [super resignFirstResponder];
  if (resigned && _owner != nullptr) {
    MomoContentView* __weak weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      MomoContentView* view = weakSelf;
      if (view == nil || view->_owner == nullptr || view.window == nil) {
        return;
      }
      MacWindow* owner = view->_owner;
      const int64_t view_id =
          [owner->platform_views() viewIdForResponder:view.window.firstResponder];
      if (view_id != 0) {
        owner->delegate()->OnPlatformViewFocused(view_id);
      }
    });
  }
  return resigned;
}
- (BOOL)isOpaque {
  return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
  return YES;
}

// ---- 几何与 surface ----

- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  if (_owner != nullptr && _owner->metal_surface() != nullptr) {
    _owner->metal_surface()->UpdateGeometry();
    if (self.inLiveResize) {
      // 拖拽缩放: 等一帧新尺寸的内容, 与新窗口尺寸在同一事务里提交(否则旧帧被拉伸到松手)。
      _owner->SynchronizeLiveResize();
    }
  }
}

- (void)viewWillStartLiveResize {
  [super viewWillStartLiveResize];
  if (_owner != nullptr && _owner->metal_surface() != nullptr) {
    _owner->metal_surface()->SetLiveResizing(true);
  }
}

- (void)viewDidEndLiveResize {
  [super viewDidEndLiveResize];
  if (_owner != nullptr && _owner->metal_surface() != nullptr) {
    _owner->metal_surface()->SetLiveResizing(false);
  }
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (_owner != nullptr) {
    if (_owner->metal_surface() != nullptr) {
      _owner->metal_surface()->UpdateGeometry();
    }
    _owner->NotifyMetricsChanged();
  }
}

// ---- 指针 ----
// 鼠标事件由窗口的 -sendEvent: 直接交给 MacWindow::SendEvent(见下方"指针"), 不经响应者投递。

- (NSPoint)viewPointFromScreen:(NSPoint)screen {
  return [self convertPoint:[self.window convertPointFromScreen:FlipScreenY(screen)] fromView:nil];
}

- (BOOL)mouseIsInside {
  const NSPoint p = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
  return NSPointInRect(p, self.bounds);
}

// AppKit 的响应者投递(含跟踪区域、响应链冒泡): 全部由派发器接管, 这里不处理也不再向上转发。
- (void)mouseDown:(NSEvent*)event {}
- (void)mouseUp:(NSEvent*)event {}
- (void)rightMouseDown:(NSEvent*)event {}
- (void)rightMouseUp:(NSEvent*)event {}
- (void)otherMouseDown:(NSEvent*)event {}
- (void)otherMouseUp:(NSEvent*)event {}
- (void)mouseDragged:(NSEvent*)event {}
- (void)rightMouseDragged:(NSEvent*)event {}
- (void)otherMouseDragged:(NSEvent*)event {}
- (void)mouseMoved:(NSEvent*)event {}
- (void)mouseEntered:(NSEvent*)event {}
- (void)mouseExited:(NSEvent*)event {}
- (void)scrollWheel:(NSEvent*)event {}

// ---- 光标 ----

- (void)setMomoCursor:(NSCursor*)cursor hidden:(BOOL)hidden {
  if (cursor != nil) {
    _cursor = cursor;
  }
  _wantCursorHidden = hidden;
  [self refreshCursor];
}

// 系统的边缘缩放区(本视图坐标): 可缩放、非全屏的窗口内侧, 各边宽度由应用配置(momo/window setResizeBorder 的
// border, 缺省 5 点)。这里的光标归系统(显示为缩放形状), 左键按下也先交给 AppKit(MacWindow::SendEvent)——
// 光标与点击按同一个判断, 用户看到缩放光标就能缩放。
- (BOOL)inSystemResizeZone:(NSPoint)p {
  NSWindow* window = self.window;
  if (_owner == nullptr || window == nil || (window.styleMask & NSWindowStyleMaskResizable) == 0 ||
      (window.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return NO;
  }
  const NSRect b = self.bounds;
  if (!NSPointInRect(p, b)) {
    return NO;
  }
  const momo::embedder::WindowResizeOptions o = _owner->resize_options();
  // 本视图不翻转: y 从底边向上。
  return p.x - NSMinX(b) < o.border_left || NSMaxX(b) - p.x < o.border_right ||
         p.y - NSMinY(b) < o.border_bottom || NSMaxY(b) - p.y < o.border_top;
}

- (BOOL)eventInSystemResizeZone:(NSEvent*)event {
  return [self inSystemResizeZone:[self convertPoint:event.locationInWindow fromView:nil]];
}

- (void)refreshCursor {
  BOOL inside = [self mouseIsInside];
  if (inside && _owner != nullptr && _owner->PointerOwner().value_or(0) != 0) {
    // 指针归平台视图: 光标归原生视图管理(如 WKWebView 的 I-beam / 手形)。
    inside = NO;
  }
  const BOOL hide = _wantCursorHidden && inside && _owner != nullptr;
  if (hide != _cursorHideApplied) {
    if (hide) {
      [NSCursor hide];
    } else {
      [NSCursor unhide];
    }
    _cursorHideApplied = hide;
  }
  if (inside && !hide) {
    const NSPoint p = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
    if ([self inSystemResizeZone:p]) {
      return;  // 系统的边缘缩放区: 光标归系统
    }
    [_cursor set];
  }
}

- (void)cursorUpdate:(NSEvent*)event {
  [self refreshCursor];
}

// ---- 键盘 ----

- (void)dispatchKey:(NSEvent*)event type:(KeyEventType)type {
  if (_owner != nullptr) {
    _owner->delegate()->OnKeyData(momo::embedder::macos::KeyDataFromKeyEvent(event, type));
  }
}

- (void)keyDown:(NSEvent*)event {
  [self dispatchKey:event type:event.isARepeat ? KeyEventType::kRepeat : KeyEventType::kDown];
  // 文本输入会话期间同时交给输入法(insertText / setMarkedText / doCommandBySelector)。
  // 是否把同一按键既当字符又当快捷键, 由框架层判断。
  if (_owner != nullptr && _owner->mac_text_input().active()) {
    [self interpretKeyEvents:@[ event ]];
  }
}

- (void)keyUp:(NSEvent*)event {
  // 同一个 NSEvent 对象既可能经本地监视器补发, 也可能被 AppKit 正常投递: 只处理一次。
  if (event == _lastKeyUp) {
    return;
  }
  _lastKeyUp = event;
  [self dispatchKey:event type:KeyEventType::kUp];
}

- (void)flagsChanged:(NSEvent*)event {
  for (const KeyData& data : momo::embedder::macos::KeyDataFromFlagsChanged(event)) {
    if (_owner == nullptr) {
      return;  // 上报过程中窗口被关闭
    }
    _owner->delegate()->OnKeyData(data);
  }
}

// ---- NSTextInputClient(委托 MacTextInput) ----

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
  if (_owner != nullptr) {
    _owner->mac_text_input().InsertText(string, replacementRange);
  }
}

- (void)doCommandBySelector:(SEL)selector {
  // 由按键派生的编辑命令(退格、方向键、回车……)不再上报: 同一按键已经以 KeyData 送达框架,
  // 框架的 EditableText 统一按键处理(跨平台一致);再发 kCommand 会导致执行两次。
  // kCommand 保留给没有对应按键事件的命令来源(见 embedder/common/text_input.h)。
  (void)selector;
}

- (void)setMarkedText:(id)string
        selectedRange:(NSRange)selectedRange
     replacementRange:(NSRange)replacementRange {
  if (_owner != nullptr) {
    _owner->mac_text_input().SetMarkedText(string, selectedRange, replacementRange);
  }
}

- (void)unmarkText {
  if (_owner != nullptr) {
    _owner->mac_text_input().UnmarkText();
  }
}

- (NSRange)selectedRange {
  return _owner != nullptr ? _owner->mac_text_input().SelectedRange() : NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange {
  return _owner != nullptr ? _owner->mac_text_input().MarkedRange() : NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText {
  return _owner != nullptr && _owner->mac_text_input().HasMarkedText();
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range
                                               actualRange:(NSRangePointer)actualRange {
  return _owner != nullptr ? _owner->mac_text_input().AttributedSubstring(range, actualRange) : nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
  return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
  return _owner != nullptr
             ? _owner->mac_text_input().FirstRectForCharacterRange(range, actualRange)
             : NSZeroRect;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
  return NSNotFound;  // 命中测试由 JS 框架负责
}

@end

// ---------------------------------------------------------------------------
// 原生窗口: 普通窗口 MomoWindow(NSWindow)与非激活面板 MomoPanel(NSPanel + NSWindowStyleMaskNonactivatingPanel,
// 同 Electron 的 type: 'panel';NonactivatingPanel 只对 NSPanel 有效)。面板可成为 key window 接收键盘, 但不成为
// main window, 点击/聚焦它也不激活应用。两者只把事件交给 owner(MacWindow, 析构时清空), 行为都在 MacWindow。

@protocol MomoNativeWindow <MomoMouseRouting>
@property(nonatomic, assign) MacWindow* owner;
@end

@interface MomoWindow : NSWindow <MomoNativeWindow>
@end

@interface MomoPanel : NSPanel <MomoNativeWindow>
@end

@implementation MomoWindow
@synthesize owner;
- (BOOL)canBecomeKeyWindow {
  return YES;  // 隐藏标题栏后也能接收键盘
}
- (void)sendEvent:(NSEvent*)event {
  if (MacWindow* window = self.owner) {
    window->SendEvent(event, ^(NSEvent* e) { [super sendEvent:e]; });
  } else {
    [super sendEvent:event];
  }
}
- (void)momoDeliverEvent:(NSEvent*)event toPlatformView:(int64_t)viewId {
  if (MacWindow* window = self.owner) {
    window->DeliverToPlatformView(event, viewId, ^(NSEvent* e) { [super sendEvent:e]; });
  }
}
- (int64_t)momoRoutedPlatformView {
  MacWindow* window = self.owner;
  return window != nullptr ? window->routed_platform_view() : 0;
}
@end

@implementation MomoPanel
@synthesize owner;
- (BOOL)canBecomeKeyWindow {
  return YES;
}
- (BOOL)canBecomeMainWindow {
  return NO;  // 应用主窗口保持 main 状态
}
- (void)sendEvent:(NSEvent*)event {
  if (MacWindow* window = self.owner) {
    window->SendEvent(event, ^(NSEvent* e) { [super sendEvent:e]; });
  } else {
    [super sendEvent:event];
  }
}
- (void)momoDeliverEvent:(NSEvent*)event toPlatformView:(int64_t)viewId {
  if (MacWindow* window = self.owner) {
    window->DeliverToPlatformView(event, viewId, ^(NSEvent* e) { [super sendEvent:e]; });
  }
}
- (int64_t)momoRoutedPlatformView {
  MacWindow* window = self.owner;
  return window != nullptr ? window->routed_platform_view() : 0;
}
@end

@interface MomoWindowObserver : NSObject <NSWindowDelegate>
- (instancetype)initWithOwner:(MacWindow*)owner;
@end

@implementation MomoWindowObserver {
  MacWindow* _owner;
}
- (instancetype)initWithOwner:(MacWindow*)owner {
  if ((self = [super init])) {
    _owner = owner;
  }
  return self;
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
  return _owner->delegate()->OnCloseRequested() ? YES : NO;
}
- (void)windowDidResize:(NSNotification*)notification {
  _owner->LayoutTrafficLights();
  _owner->NotifyMetricsChanged();
  _owner->NotifyFrameChanged();
  _owner->NotifyControlsChanged();
  _owner->NotifyStateChanged();  // 缩放(zoom)
}
- (void)windowDidMove:(NSNotification*)notification {
  _owner->NotifyFrameChanged();
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
  _owner->NotifyControlsChanged();  // 全屏时红绿灯不占用内容区
  _owner->NotifyStateChanged();
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
  _owner->LayoutTrafficLights();
  _owner->NotifyControlsChanged();
  _owner->NotifyStateChanged();
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification {
  _owner->NotifyMetricsChanged();
}
- (void)windowDidChangeScreen:(NSNotification*)notification {
  _owner->NotifyMetricsChanged();
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
  _owner->delegate()->OnFocusChanged(true);
}
- (void)windowDidResignKey:(NSNotification*)notification {
  _owner->delegate()->OnFocusChanged(false);
}
- (void)windowDidMiniaturize:(NSNotification*)notification {
  _owner->NotifyStateChanged();
}
- (void)windowDidDeminiaturize:(NSNotification*)notification {
  _owner->NotifyStateChanged();
}
@end

namespace momo::embedder::macos {

MacWindow::MacWindow(const WindowSpec& spec, WindowDelegate* delegate, MacMouseInput& mouse)
    : delegate_(delegate), mouse_(mouse) {
  NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable;
  resize_options_.resizable = spec.resizable;
  if (spec.resizable) {
    style |= NSWindowStyleMaskResizable;
  }
  const NSRect frame = NSMakeRect(0, 0, spec.size.width, spec.size.height);
  NSWindow<MomoNativeWindow>* window = nil;
  if (spec.non_activating) {
    // 面板: 不激活应用即可成为 key(NonactivatingPanel 只对 NSPanel 有效);应用失活时不隐藏;
    // 层级由 SetAlwaysOnTop 决定(不默认浮动)。
    MomoPanel* panel = [[MomoPanel alloc] initWithContentRect:frame
                                                    styleMask:style | NSWindowStyleMaskNonactivatingPanel
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
    panel.hidesOnDeactivate = NO;
    panel.floatingPanel = NO;
    panel.becomesKeyOnlyIfNeeded = NO;
    window = panel;
  } else {
    window = [[MomoWindow alloc] initWithContentRect:frame
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
  }
  non_activating_ = spec.non_activating;
  window_ = window;
  window_.releasedWhenClosed = NO;  // 生命周期由本对象(ARC)管理
  window_.title = ToNSString(spec.title);
  window_.contentMinSize = NSMakeSize(spec.min_size.width, spec.min_size.height);
  window_.backgroundColor = ToNSColor(spec.background_argb);

  view_ = [[MomoContentView alloc] initWithFrame:frame];
  view_.wantsLayer = YES;
  view_.layer.backgroundColor = ToNSColor(spec.background_argb).CGColor;
  // 软件路径: 内容(IOSurface)铺满视图;尺寸与 backing 像素一致时为 1:1, 拖拽缩放期间先拉伸, 下一帧即修正。
  view_.layer.contentsGravity = kCAGravityResize;
  view_.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
  window_.contentView = view_;

  // Surface: Metal 优先;强制软件或无 Metal 设备时用 IOSurface 软件 surface。
  if (SoftwareRenderingForced()) {
    MOMO_LOG(INFO) << "MOMO_SOFTWARE_RENDERING set: using the software surface.";
  } else {
    StatusOr<std::unique_ptr<MacMetalSurface>> metal = MacMetalSurface::Create(view_);
    if (metal.ok()) {
      metal_surface_ = metal->get();
      surface_ = std::move(metal).value();
    } else {
      MOMO_LOG(WARNING) << "Metal surface unavailable (" << metal.status()
                        << "); using the software surface.";
    }
  }
  if (surface_ == nullptr) {
    surface_ = std::make_unique<MacSoftwareSurface>(view_);
  }

  // 平台视图宿主(只在 Metal surface 上支持合成;软件 surface 下视图保持隐藏)。
  platform_views_ = [[MomoPlatformViews alloc] initWithContentView:view_];
  if (metal_surface_ != nullptr) {
    metal_surface_->SetPlatformViews(platform_views_);
  }
  input_router_ = [[MomoPlatformViewInputRouter alloc] initWithPlatformViews:platform_views_
                                                                 contentView:view_];
  MomoContentView* __weak weak_view = view_;
  MacMouseInput* mouse_input = &mouse_;  // 比窗口活得久
  // 几何变化(新的一帧已呈现、视图移除): 在呈现事务之外(下一轮主循环)请派发器重新判定指针下的接收方。
  platform_views_.geometryChanged = ^{
    dispatch_async(dispatch_get_main_queue(), ^{
      if (MacWindow* owner = weak_view.owner) mouse_input->dispatcher().GeometryChanged(owner);
    });
  };
  platform_views_.pointerOwner = ^int64_t {
    MacWindow* owner = weak_view.owner;
    return owner != nullptr ? owner->PointerOwner().value_or(-1) : -1;
  };

  text_input_ = std::make_unique<MacTextInput>(view_, delegate_);
  [view_ attachOwner:this];
  window.owner = this;
  mouse_.AddWindow(window_);
  [window_ makeFirstResponder:view_];

  observer_ = [[MomoWindowObserver alloc] initWithOwner:this];
  window_.delegate = observer_;
  if (spec.position) {
    SetPosition(*spec.position).IgnoreError();
  } else {
    [window_ center];
  }
  last_metrics_ = metrics();
  last_state_ = state();  // 尚未显示
}

MacWindow::~MacWindow() {
  dispatcher().RemoveWindow(this);
  mouse_.RemoveWindow(window_);
  ((NSWindow<MomoNativeWindow>*)window_).owner = nullptr;
  [view_ detachOwner];  // 之后的事件/IME 回调不再触达本对象
  window_.delegate = nil;
  [window_ orderOut:nil];
  [window_ close];
  surface_.reset();  // raster 已解绑(Shell::DetachView 先于窗口销毁)
  metal_surface_ = nullptr;
  platform_views_ = nil;  // 平台视图已由 Shell::DetachView 移出
  input_router_ = nil;
  text_input_.reset();
  observer_ = nil;
  view_ = nil;
  window_ = nil;
}

void MacWindow::Show() {
  if (non_activating_) {
    // 面板: 带到最前并成为 key, 但不激活应用(其他窗口保持原状)。
    [window_ orderFrontRegardless];
    [window_ makeKeyWindow];
  } else {
    [window_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
  }
  NotifyStateChanged();
}

WindowStateFlags MacWindow::state() const {
  WindowStateFlags s;
  s.minimized = window_.miniaturized;
  s.visible = window_.visible || s.minimized;  // 最小化的窗口 isVisible 为 NO, 但没有被隐藏
  s.fullscreen = (window_.styleMask & NSWindowStyleMaskFullScreen) != 0;
  s.maximized = !s.fullscreen && resize_options_.resizable && window_.zoomed;
  return s;
}

void MacWindow::NotifyStateChanged() {
  const WindowStateFlags s = state();
  if (s == last_state_) {
    return;
  }
  last_state_ = s;
  delegate_->OnWindowStateChanged(s);
}

Status MacWindow::Hide() {
  [window_ orderOut:nil];
  NotifyStateChanged();
  return Status::Ok();
}

Status MacWindow::Minimize() {
  [window_ miniaturize:nil];  // windowDidMiniaturize 上报状态
  return Status::Ok();
}

Status MacWindow::Maximize() {
  if (window_.miniaturized) {
    [window_ deminiaturize:nil];
  }
  if (!window_.zoomed) {
    [window_ zoom:nil];
  }
  NotifyStateChanged();
  return Status::Ok();
}

Status MacWindow::Restore() {
  if (window_.miniaturized) {
    [window_ deminiaturize:nil];
  } else if (window_.zoomed) {
    [window_ zoom:nil];
  }
  NotifyStateChanged();
  return Status::Ok();
}

void MacWindow::SetTitle(std::string_view title) { window_.title = ToNSString(title); }

void MacWindow::SetMinSize(Size logical_size) {
  window_.contentMinSize = NSMakeSize(logical_size.width, logical_size.height);
}

Size MacWindow::physical_size() const {
  const NSRect backing = [view_ convertRectToBacking:view_.bounds];
  return {backing.size.width, backing.size.height};
}

double MacWindow::device_pixel_ratio() const { return window_.backingScaleFactor; }

platform::RenderSurface& MacWindow::surface() { return *surface_; }

void MacWindow::SetCursor(SystemCursor cursor) {
  NSCursor* ns_cursor = nil;
  switch (cursor) {
    case SystemCursor::kArrow: ns_cursor = [NSCursor arrowCursor]; break;
    case SystemCursor::kIBeam: ns_cursor = [NSCursor IBeamCursor]; break;
    case SystemCursor::kPointingHand: ns_cursor = [NSCursor pointingHandCursor]; break;
    case SystemCursor::kCrosshair: ns_cursor = [NSCursor crosshairCursor]; break;
    case SystemCursor::kResizeLeftRight: ns_cursor = [NSCursor resizeLeftRightCursor]; break;
    case SystemCursor::kResizeUpDown: ns_cursor = [NSCursor resizeUpDownCursor]; break;
    case SystemCursor::kNotAllowed: ns_cursor = [NSCursor operationNotAllowedCursor]; break;
    case SystemCursor::kHidden: break;
  }
  [view_ setMomoCursor:ns_cursor hidden:cursor == SystemCursor::kHidden];
}

TextInputConnection& MacWindow::text_input() { return *text_input_; }

uint32_t MacWindow::capabilities() const {
  return kCapabilityAlwaysOnTop | kCapabilityPosition | kCapabilityResizeOptions |
         kCapabilityHiddenTitleBar | kCapabilityWindowDrag | kCapabilityMoveLoop |
         kCapabilityPointerCapture | kCapabilityPlatformViews | kCapabilityNonActivating |
         kCapabilityVisibility | kCapabilityMinMax;
}

void* MacWindow::native_handle() const { return (__bridge void*)view_; }

Status MacWindow::SetAlwaysOnTop(bool on) {
  window_.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
  // 置顶窗口跟随用户切换到的桌面空间, 并可浮在全屏应用之上。
  window_.collectionBehavior = on ? (NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorFullScreenAuxiliary)
                                  : NSWindowCollectionBehaviorDefault;
  return Status::Ok();
}

Status MacWindow::SetResizeOptions(const WindowResizeOptions& options) {
  resize_options_ = options;
  NSWindowStyleMask style = window_.styleMask;
  style = options.resizable ? (style | NSWindowStyleMaskResizable) : (style & ~NSWindowStyleMaskResizable);
  if (style != window_.styleMask) {
    window_.styleMask = style;
  }
  [view_ refreshCursor];
  return Status::Ok();
}

Status MacWindow::SetTitleBar(const TitleBarOptions& options) {
  title_bar_ = options;
  const bool hidden = options.hidden;
  if (hidden) {
    window_.styleMask |= NSWindowStyleMaskFullSizeContentView;
    window_.titlebarAppearsTransparent = YES;
    window_.titleVisibility = NSWindowTitleHidden;
  } else {
    window_.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
    window_.titlebarAppearsTransparent = NO;
    window_.titleVisibility = NSWindowTitleVisible;
  }
  LayoutTrafficLights();
  NotifyMetricsChanged();  // 内容区包含/不含标题栏后尺寸变化
  NotifyControlsChanged();
  return Status::Ok();
}

std::optional<Rect> MacWindow::window_controls_rect() const {
  if (!title_bar_.hidden || (window_.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return std::nullopt;  // 系统标题栏可见(按钮不在内容区里)或全屏(按钮隐藏)
  }
  NSRect bounds = NSZeroRect;
  for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    NSButton* button = [window_ standardWindowButton:kind];
    if (button == nil || button.hidden || button.superview == nil) continue;
    const NSRect r = [view_ convertRect:button.bounds fromView:button];
    bounds = NSIsEmptyRect(bounds) ? r : NSUnionRect(bounds, r);
  }
  if (NSIsEmptyRect(bounds)) {
    return std::nullopt;
  }
  // 本视图不翻转(左下原点)→ 内容区左上原点。左右各留出与左边距相同的空白, 应用据此避让。
  const CGFloat margin = NSMinX(bounds);
  return Rect{0, NSHeight(view_.bounds) - NSMaxY(bounds), NSMaxX(bounds) + margin, NSHeight(bounds)};
}

void MacWindow::NotifyControlsChanged() {
  const std::optional<Rect> controls = window_controls_rect();
  if (controls == last_controls_) {
    return;
  }
  last_controls_ = controls;
  delegate_->OnWindowControlsChanged(controls);
}

void MacWindow::LayoutTrafficLights() {
  const double traffic_center_y = title_bar_.height / 2;
  if (!title_bar_.hidden || traffic_center_y <= 0 ||
      (window_.styleMask & NSWindowStyleMaskFullScreen) != 0) {
    return;
  }
  NSButton* close = [window_ standardWindowButton:NSWindowCloseButton];
  NSView* titlebar = close.superview;         // NSTitlebarView
  NSView* container = titlebar.superview;     // NSTitlebarContainerView
  if (close == nil || container == nil) {
    return;
  }
  // 把标题栏容器的高度设为应用标题栏的高度, 按钮在其中垂直居中。
  const CGFloat height = static_cast<CGFloat>(traffic_center_y * 2);
  NSRect container_frame = container.frame;
  container_frame.size.height = height;
  container_frame.origin.y = NSHeight(window_.frame) - height;
  container.frame = container_frame;
  titlebar.frame = container.bounds;
  for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
    NSButton* button = [window_ standardWindowButton:kind];
    NSRect f = button.frame;
    f.origin.y = (height - f.size.height) / 2;
    button.frame = f;
  }
}

// 屏幕框与定位都以内容区(momo 视图)为准: 屏幕坐标 − screen_frame 即窗口内坐标, 与窗口样式无关。
Rect MacWindow::screen_frame() const {
  return ScreenRectFromCocoa([window_ contentRectForFrameRect:window_.frame]);
}

Status MacWindow::SetPosition(Point top_left) {
  // 内容区左上角 → 窗口外框左上角(内容区之上可能还有标题栏)。
  const NSRect frame = window_.frame;
  const NSRect content = [window_ contentRectForFrameRect:frame];
  const CGFloat titlebar = NSMaxY(frame) - NSMaxY(content);
  [window_ setFrameTopLeftPoint:NSMakePoint(top_left.x - (content.origin.x - frame.origin.x),
                                            PrimaryScreenTop() - top_left.y + titlebar)];
  return Status::Ok();
}

void MacWindow::NotifyFrameChanged() {
  const Rect frame = screen_frame();
  if (frame == last_frame_) {
    return;
  }
  last_frame_ = frame;
  delegate_->OnWindowFrameChanged(frame);
}

Status MacWindow::StartWindowDrag(int64_t sequence_id) {
  if (!dispatcher().IsMomoSequence(this, sequence_id)) {
    return Status::Ok();  // 序列已结束(往返期间用户已放手)或已不归本窗口
  }
  NSEvent* down = sequence_down_;
  if (sequence_down_id_ != sequence_id || down == nil || down.type != NSEventTypeLeftMouseDown) {
    return InvalidArgumentError("startDrag: the sequence did not start with a left press in this window");
  }
  if (down.clickCount == 2) {
    TitleBarDoubleClick(window_);
    return Status::Ok();
  }
  // 系统拖动(含贴边、跨屏等系统行为)。拖动循环吞掉抬起: 返回时按键已松开则在这里结束序列
  // (否则由派发器的过期判定在下一个事件上结束)。
  [window_ performWindowDragWithEvent:down];
  if (([NSEvent pressedMouseButtons] & 1) == 0) {
    const NSPoint screen = FlipScreenY(NSEvent.mouseLocation);
    dispatcher().EndSequence(this, {screen.x, screen.y},
                             static_cast<int64_t>(NSProcessInfo.processInfo.systemUptime * 1e6));
  }
  return Status::Ok();
}

Status MacWindow::StartMoveLoop(Point grab_offset) {
  dispatcher().StartMoveLoop(this, grab_offset);  // 不是进行中序列的接收方时什么也不做
  return Status::Ok();
}

void MacWindow::EndMoveLoop() { dispatcher().EndMoveLoop(this); }

void MacWindow::Activate() {
  if (window_.miniaturized) {
    [window_ deminiaturize:nil];
  }
  Show();
}

StatusOr<bool> MacWindow::CapturePointer(int64_t sequence_id) {
  return dispatcher().Capture(this, sequence_id);
}

void MacWindow::ReleasePointerCapture() { dispatcher().ReleaseCapture(this); }

// ---- 指针 ----
// 决定接收方的是 PointerDispatcher(embedder/common/pointer_dispatcher.h, 进程内一个, 由 MacMouseInput 持有):
// 这里只把 NSEvent 交给 MacMouseInput 翻译后转给它;它经本类(PointerWindow)把事件交给 momo 内容或平台视图
// (MomoPlatformViewInputRouter)。命中依据每一帧与画面同一个 CATransaction 生效的几何(HitTest), 即用户此刻
// 看到的画面, 不经 ui 线程往返。

PointerDispatcher& MacWindow::dispatcher() const { return mouse_.dispatcher(); }

std::optional<int64_t> MacWindow::PointerOwner() const { return dispatcher().PointerOwner(this); }

// 鼠标与其他位置型输入的入口(docs/05 §9 "输入"): 交给派发器决定接收方(momo 内容或平台视图), 不经 AppKit
// 投递。只有窗口装饰(红绿灯、边缘缩放区)的按下序列与它们自己的跟踪区事件直接交给 AppKit。窗口拖动由应用在
// 按下时请求(momo/window startDrag), StartWindowDrag 以该序列的按下开始系统拖动。
void MacWindow::SendEvent(NSEvent* event, MomoSuperSendEvent super_send) {
  const MomoMouseEventKind kind = MomoClassifyMouseEvent(event);
  // 跟踪区事件(进入/离开/光标更新)按归属分三类:
  //   原生视图(平台视图)的 → 不投递, 原生视图的跟踪状态由有序输入流推导(MomoPlatformViewInputRouter);
  //   momo 内容视图的 → 派发器;
  //   窗口装饰(主题框架、标题栏、红绿灯)的 → AppKit, 它们维护窗口级状态, 不参与归属。
  const bool tracking = event.type == NSEventTypeCursorUpdate || kind == MomoMouseEventKindEnter ||
                        kind == MomoMouseEventKindExit;
  if (tracking && event.trackingArea != nil && [input_router_ routeTrackingEvent:event]) {
    return;
  }
  if (kind == MomoMouseEventKindNone) {
    super_send(event);
    return;
  }
  // 窗口控件(红绿灯)优先于 momo: 整个按下序列交给 AppKit。
  if (system_tracking_) {
    super_send(event);
    if (kind == MomoMouseEventKindUp) system_tracking_ = false;
    return;
  }
  if (kind == MomoMouseEventKindDown && !dispatcher().has_sequence()) {
    if (event.type == NSEventTypeLeftMouseDown && HitsWindowControl(FrameHit(view_, event))) {
      system_tracking_ = true;
      super_send(event);
      return;
    }
    if (event.type == NSEventTypeLeftMouseDown && [view_ eventInSystemResizeZone:event]) {
      // 边缘缩放区(光标在这里由系统显示为缩放形状): 先交给 AppKit, 优先于 momo 内容。AppKit 在
      // -[NSWindow sendEvent:] 内跑缩放的跟踪循环直到松开;是否真的是缩放由它决定——松开后按键已不再按下,
      // 说明它接管了整个按下序列。否则(不在 AppKit 的缩放区)照常往下, 交给 momo。
      // 试探期间命中测试不落到任何平台视图(kNoPlatformView), 第一响应者保持不变: AppKit 若不缩放, 会把按下
      // 投递给命中的视图——不能让原生视图或焦点绕过 momo 的路线。
      NSResponder* responder = window_.firstResponder;
      const int64_t routed = routed_platform_view_;
      routed_platform_view_ = kNoPlatformView;
      super_send(event);
      routed_platform_view_ = routed;
      if (([NSEvent pressedMouseButtons] & 1) == 0) {
        return;
      }
      if (window_.firstResponder != responder) [window_ makeFirstResponder:responder];
    }
  }
  if ((kind == MomoMouseEventKindEnter || kind == MomoMouseEventKindExit) &&
      event.trackingArea.owner != view_) {
    super_send(event);  // 窗口装饰自己的跟踪区域(原生视图的已在上面处理)
    return;
  }
  // 交给派发器, 再完成点击激活: 按下不经 AppKit, 窗口激活由这里完成(面板只成为 key, 不激活应用)。
  mouse_.Dispatch(event, this);
  switch (kind) {
    case MomoMouseEventKindDown:
      ActivateForMomoClick();
      break;
    case MomoMouseEventKindMove:
      // 窗口装饰照常记账;光标归属: 装饰在处理中设置了光标(缩放区)则归装饰, 否则重新应用 momo 的光标
      // (指针归平台视图时由原生视图管理, 见 refreshCursor)。先应用 momo 的光标, 装饰改动与否才可比较。
      [view_ refreshCursor];
      if (!ForwardMoveToWindowChrome(view_, event)) [view_ refreshCursor];
      break;
    case MomoMouseEventKindDrag:
    case MomoMouseEventKindEnter:
    case MomoMouseEventKindExit:
      [view_ refreshCursor];
      break;
    default:
      break;
  }
}

// 直接调用父类实现(不经 SendEvent 分流): 投递期间宿主的命中测试只落在该平台视图的原生视图子树里,
// AppKit 照常完成按下视图的捕获、第一响应者、手势识别器、immediate action 等。可重入(原生视图在处理中
// 跑嵌套事件循环时, 循环里的事件经 -sendEvent: 重新进入 momo 的路线)。
void MacWindow::DeliverToPlatformView(NSEvent* event, int64_t view_id, MomoSuperSendEvent super_send) {
  const int64_t previous = routed_platform_view_;
  routed_platform_view_ = view_id;
  super_send(event);
  routed_platform_view_ = previous;
}

void MacWindow::ActivateForMomoClick() {
  if (window_.isKeyWindow || !window_.canBecomeKeyWindow) {
    return;
  }
  if (non_activating_) {
    [window_ makeKeyWindow];
    return;
  }
  [window_ makeKeyAndOrderFront:nil];
  if (!NSApp.isActive) [NSApp activateIgnoringOtherApps:YES];
}

// ---- PointerWindow ----

std::optional<int64_t> MacWindow::HitTest(Point screen) const {
  if (!window_.visible) {
    return std::nullopt;
  }
  // 被本应用的别的窗口遮住的地方不属于本窗口(重叠的窗口之间只有最上层的那个接收指针)。别的应用的窗口
  // 盖住时系统根本不把事件交给本应用, 不必考虑。
  const NSPoint cocoa = FlipScreenY(NSMakePoint(screen.x, screen.y));
  for (NSNumber* number in [NSWindow windowNumbersWithOptions:0]) {  // 本应用的屏幕上窗口, 由前到后
    NSWindow* w = [NSApp windowWithWindowNumber:number.integerValue];
    if (w != nil && !w.ignoresMouseEvents && NSPointInRect(cocoa, w.frame)) {
      if (w != window_) return std::nullopt;
      break;
    }
  }
  const NSPoint in_window = [window_ convertPointFromScreen:cocoa];
  if (!NSPointInRect([view_ convertPoint:in_window fromView:nil], view_.bounds)) {
    return std::nullopt;
  }
  return [platform_views_ platformViewAtWindowPoint:in_window];
}

void MacWindow::SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) {
  // 屏幕坐标 → 本视图的物理像素(左上原点);滚动位移 点 → 物理像素。
  const double scale = device_pixel_ratio() > 0 ? device_pixel_ratio() : 1.0;
  const CGFloat height = view_.bounds.size.height;
  for (platform::PointerData& data : packet) {
    const NSPoint p = [view_ viewPointFromScreen:NSMakePoint(data.screen_x, data.screen_y)];
    data.physical_x = p.x * scale;
    data.physical_y = (height - p.y) * scale;
    data.scroll_delta_x *= scale;
    data.scroll_delta_y *= scale;
    if (data.phase == PointerPhase::kDown) {
      // 序列的按下(startDrag 据此开始系统拖动)。按下由 momo 内容接收: 键盘焦点交还 momo(之前可能在网页里)。
      sequence_down_ = (__bridge NSEvent*)input.native;
      sequence_down_id_ = data.sequence_id;
      if ([platform_views_ viewIdForResponder:window_.firstResponder] != 0) {
        [window_ makeFirstResponder:view_];
      }
    }
  }
  delegate_->OnPointerPacket(std::move(packet));
}

void MacWindow::SendToPlatformView(int64_t view, const PointerInput& input, PointerPhase phase) {
  NSEvent* event = (__bridge NSEvent*)input.native;
  if (event != nil) {
    [input_router_ deliverEvent:event toPlatformView:view phase:phase windowWasKey:window_.isKeyWindow];
  }
}

void MacWindow::SetHoveredPlatformView(int64_t view, const PointerInput& input) {
  // 别的窗口的事件的窗口坐标不属于本窗口: 取当前指针位置。
  NSEvent* event = input.window == this ? (__bridge NSEvent*)input.native : nil;
  [input_router_ setHoveredPlatformView:view event:event];
  [view_ refreshCursor];
}

void MacWindow::MoveTo(Point top_left) { SetPosition(top_left).IgnoreError(); }

void MacWindow::AddPlatformView(int64_t view_id, void* native_view) {
  id object = (__bridge id)native_view;
  if (![object isKindOfClass:[NSView class]]) {
    MOMO_LOG(ERROR) << "Platform view " << view_id << " is not an NSView; ignored.";
    return;
  }
  [platform_views_ addView:(NSView*)object viewId:view_id];
}

void MacWindow::RemovePlatformView(int64_t view_id) {
  [platform_views_ removeViewId:view_id];
  [input_router_ forgetPlatformView:view_id];
}

void MacWindow::FocusPlatformView(int64_t view_id) { [platform_views_ focusViewId:view_id]; }

void MacWindow::SynchronizeLiveResize() {
  // 嵌套(等待期间执行的平台任务又改变了窗口尺寸)时不再等待, 由外层的等待覆盖。
  // 等待上限: 应用在此之内出帧则缩放无拉伸, 否则暂时拉伸旧内容(窗口拖动保持跟手)。
  constexpr TimeDelta kLiveResizeFrameWait = std::chrono::milliseconds(100);
  if (metal_surface_ == nullptr || resize_waiting_ || !metal_surface_->has_presented()) {
    NotifyMetricsChanged();
    return;
  }
  resize_waiting_ = true;
  // 先把新尺寸交给引擎(windowDidResize 随后的通知会被去重), 再等它画出这个尺寸的帧。
  NotifyMetricsChanged();
  EventLoop* loop = EventLoop::Current();
  const bool got_frame = metal_surface_->WaitForFrame(
      physical_size(), kLiveResizeFrameWait, [loop] {
        if (loop != nullptr) {
          loop->RunDueTasksNested();
        }
      });
  if (!got_frame) {
    MOMO_LOG(VERBOSE) << "Live resize: no frame of the new size within the timeout; content is stretched.";
  }
  resize_waiting_ = false;
}

void MacWindow::NotifyMetricsChanged() {
  const platform::ViewportMetrics current = metrics();
  if (current == last_metrics_) {
    return;
  }
  last_metrics_ = current;
  delegate_->OnMetricsChanged(current);
}

}  // namespace momo::embedder::macos
