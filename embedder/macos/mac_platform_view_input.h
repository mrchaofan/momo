#pragma once

// MomoPlatformViewInputRouter: 窗口级的平台视图输入路由(docs/05 §9 "输入")。仅被 .mm 包含。
//
// 分工(同 Chromium 的 RenderWidgetHostInputEventRouter / aura WindowEventDispatcher, Flutter 的
// FlutterMutatorView 只管几何):
//   - 谁接收: PointerDispatcher(embedder/common/pointer_dispatcher.h)在主线程按每帧的几何同步决定,
//     不经 ui 线程往返。本路由器只执行: 把原始 NSEvent 交给选中的原生视图——
//       按钮/拖拽/抬起、滚动、触控板手势、压力、手写板点: 经 AppKit 的正常投递(MomoMouseRouting),
//         投递期间窗口的命中测试只落在该原生视图的子树里;按下视图的捕获、第一响应者、手势识别器、
//         immediate action 等由 AppKit 完成。
//       移动: 按 AppKit 的跟踪区规则投递给原生视图子树中 NSTrackingMouseMoved 跟踪区的 owner
//         (AppKit 自己只把移动发给 first responder, 未聚焦的 WKWebView 收不到悬停)。
//       进入/离开/光标更新: 由有序输入流(投递的事件 + 派发器判定的悬停目标)按 AppKit 的规则推导;
//         AppKit 为原生视图跟踪区生成的这些事件不投递(与派发器的判定没有先后关系)。
//   - 平台视图本身(MomoPlatformViews 的容器)只负责几何: 裁剪、圆角、透明度、z 序。
// WKWebView 相当于 WebKit 自己的宿主 view(它装跟踪区、由 AppKit 驱动), 本路由器对它扮演 AppKit 的角色。
// 线程约定: 只在主(platform)线程使用。

#include <cstdint>

#include "engine/platform/input_types.h"
#include "engine/platform/platform_view.h"

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>

@class MomoPlatformViews;

// 窗口实现: 把原始事件经 AppKit 的正常投递(-[NSWindow sendEvent:] 的父类实现)交给平台视图 viewId。
// 投递期间 momoRoutedPlatformView 为该 id, 平台视图宿主的命中测试只落在它的原生视图子树里。
@protocol MomoMouseRouting <NSObject>
- (void)momoDeliverEvent:(NSEvent*)event toPlatformView:(int64_t)viewId;
@property(nonatomic, readonly) int64_t momoRoutedPlatformView;  // 0 = 不在投递中
@end

// momoRoutedPlatformView 的特殊值: 窗口正把事件交给 AppKit, 但不允许落到任何平台视图(试探系统缩放区等)。
inline constexpr int64_t kNoPlatformView = -1;

@interface MomoPlatformViewInputRouter : NSObject

// views: 本窗口的平台视图(几何);content: momo 内容视图(按下由 momo 消费时取回键盘焦点)。
- (instancetype)initWithPlatformViews:(MomoPlatformViews*)views contentView:(NSView*)content;

// 把原生事件投递给平台视图 viewId。phase 是派发器给出的序列语义(PointerWindow::SendToPlatformView):
// kDown 序列开始、kMove 序列内、kUp 序列结束、kHover、kScroll、kGesture。wasKey: 事件到达时窗口是否已是 key。
- (void)deliverEvent:(NSEvent*)event
      toPlatformView:(int64_t)viewId
               phase:(momo::engine::platform::PointerPhase)phase
        windowWasKey:(BOOL)wasKey;
// 指针所在的平台视图(派发器判定的悬停目标;0 = 不在平台视图上)。event 为引起变化的原生事件
// (nil = 取当前指针位置)。
- (void)setHoveredPlatformView:(int64_t)viewId event:(NSEvent*)event;

// AppKit 为平台视图跟踪区生成的进入/离开/光标更新: 认领(返回 YES)但不投递。
- (BOOL)routeTrackingEvent:(NSEvent*)event;

// 平台视图已移除: 丢弃它的输入状态。
- (void)forgetPlatformView:(int64_t)viewId;

@end

#endif  // __OBJC__
