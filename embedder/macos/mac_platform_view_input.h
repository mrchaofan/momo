#pragma once

// MomoPlatformViewInputRouter: 窗口级的平台视图输入路由(docs/05 §9 "输入")。仅被 .mm 包含。
//
// 分工(同 Chromium 的 RenderWidgetHostInputEventRouter / aura WindowEventDispatcher, Flutter 的
// FlutterMutatorView 只管几何):
//   - 谁接收: 框架(JS)命中测试与捕获决定。窗口把全部鼠标与位置型输入交给 momo, 附带本路由器分配的
//     原生事件 id;框架对落在平台视图上的事件回送 (viewId, eventId, action)。
//   - 投递: 本路由器按 id 取回原始 NSEvent, 交给原生视图——
//       按钮/拖拽/抬起、滚动、触控板手势、压力、手写板点: 经 AppKit 的正常投递(MomoMouseRouting),
//         投递期间窗口的命中测试只落在该原生视图的子树里;按下视图的捕获、第一响应者、手势识别器、
//         immediate action 等由 AppKit 完成。
//       移动: 按 AppKit 的跟踪区规则投递给原生视图子树中 NSTrackingMouseMoved 跟踪区的 owner
//         (AppKit 自己只把移动发给 first responder, 未聚焦的 WKWebView 收不到悬停)。
//       进入/离开/光标更新: 由有序输入流(回送的事件 + 框架的指针归属)按 AppKit 的规则推导;
//         AppKit 为原生视图跟踪区生成的这些事件不投递(与经 ui 线程往返的事件没有先后关系)。
//   - 平台视图本身(MomoPlatformViews 的容器)只负责几何: 裁剪、圆角、透明度、z 序。
// WKWebView 相当于 WebKit 自己的宿主 view(它装跟踪区、由 AppKit 驱动), 本路由器对它扮演 AppKit 的角色。
// 线程约定: 只在主(platform)线程使用。

#include <cstdint>

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

@interface MomoPlatformViewInputRouter : NSObject

// views: 本窗口的平台视图(几何);content: momo 内容视图(按下由 momo 消费时取回键盘焦点)。
- (instancetype)initWithPlatformViews:(MomoPlatformViews*)views contentView:(NSView*)content;

// 为交给 momo 的原生事件分配 id(窗口内递增, < 2^53)并暂存原始事件(环形缓冲)。
- (uint64_t)recordEvent:(NSEvent*)event;

// 框架对原生事件的处置(view.dispatchPlatformViewPointer)。
- (void)dispatchToPlatformView:(int64_t)viewId
                 nativeEventId:(uint64_t)eventId
                        action:(momo::engine::platform::PlatformViewPointerAction)action;

// 以下两个是 dispatch 的组成部分, 也供测试直接驱动。
// 把框架交给平台视图 viewId 的原生事件投递给原生视图。wasKey: 事件到达时窗口是否已是 key。
- (void)deliverEvent:(NSEvent*)event toPlatformView:(int64_t)viewId windowWasKey:(BOOL)wasKey;
// 指针归属(框架 MouseTracker 的进入/离开)。event 为触发它的原生事件(nil = 取当前指针位置)。
- (void)setPointerOwned:(BOOL)owned platformView:(int64_t)viewId event:(NSEvent*)event;

// AppKit 为平台视图跟踪区生成的进入/离开/光标更新: 认领(返回 YES)但不投递。
- (BOOL)routeTrackingEvent:(NSEvent*)event;

// 平台视图已移除: 丢弃它的输入状态。
- (void)forgetPlatformView:(int64_t)viewId;

// 指针当前是否归某个平台视图(光标此时由原生视图管理)。
@property(nonatomic, readonly) BOOL pointerOwned;
// 指针是否归平台视图 viewId(框架的进入/离开)。
- (BOOL)pointerOwnedByPlatformView:(int64_t)viewId;
// momo 是否掌握指针位置(指针在窗口内, 内容视图已上报 add 未上报 remove)。拖放会话等期间框架收不到
// 鼠标事件, 不掌握——此时 AppKit 的几何命中按画面上的遮挡近似(见 MomoPlatformViews)。
@property(nonatomic) BOOL pointerTracked;

@end

#endif  // __OBJC__
