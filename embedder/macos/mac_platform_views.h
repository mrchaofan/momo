#pragma once

// MomoPlatformViews: 平台视图在 macOS 窗口中的宿主(docs/05 §9)。仅被 .mm 包含。
//
// 视图层级(内容视图 MomoContentView 之下):
//   content.layer
//   ├── CAMetalLayer(主帧, MacMetalSurface;zPosition = -1)
//   └── MomoPlatformViewsHostView(铺满内容视图, flipped, 自身不可命中)
//       ├── MomoPlatformViewContainer(每个平台视图一个: 可见区域裁剪 + 原生视图)
//       └── MomoOverlayView(每个 overlay 一个: 承载一个 CAMetalLayer, 不可命中)
// z 序由各子视图 layer 的 zPosition 决定(按合成布局 base → view0 → overlay0 → view1 …), 避免为调序
// 而移除/重新加入子视图(会让 WKWebView 丢失第一响应者)。
// 命中: 鼠标事件由 MomoMouseEventDispatcher 统一派发(mac_mouse_dispatcher.h)。每个可见容器是一个
//   派发目标: 指针位于其可见区域、且不在"上层 momo 内容"(occluders)内时认领事件, 由 AppKit 投递给原生视图;
//   AppKit 命中测试期间宿主视图只返回被派发器选中的原生视图。容器与宿主吞掉沿响应链冒泡上来的鼠标事件,
//   原生视图未处理的事件不会漏给 momo。
// 线程约定: 只在主(platform)线程使用;MacMetalSurface 在主线程呈现合成帧时调用 applyLayout。

#include <cstdint>
#include <vector>

#include "engine/platform/platform_view.h"

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "embedder/macos/mac_mouse_dispatcher.h"

@interface MomoPlatformViews : NSObject

- (instancetype)initWithContentView:(NSView*)contentView;
// 视图挂入(初始隐藏)/移出。native 由扩展所有, 挂入期间本对象持有强引用。
- (void)addView:(NSView*)native viewId:(int64_t)viewId;
- (void)removeViewId:(int64_t)viewId;
- (void)focusViewId:(int64_t)viewId;
// 合成帧(调用方已开启 CATransaction)。overlayLayers 与 layout.overlays 等长, 元素可为 nil。
- (void)applyLayout:(const momo::engine::platform::CompositionLayout&)layout
      overlayLayers:(const std::vector<CAMetalLayer*>&)overlayLayers;
// 隐藏全部平台视图与 overlay(软件回退等)。
- (void)hideAll;
// 内容视图坐标(点)下的位置是否落在可见、未被遮挡的平台视图上。
- (BOOL)platformViewAtContentPoint:(NSPoint)point;
// 原生视图自己跟踪区的进入/离开/光标更新(NSEvent.trackingArea 属于某个平台视图时返回 YES 并处理):
// 原生视图的悬停状态由派发器的指针归属推导, 而不是 AppKit 的几何状态——被上层 momo 内容盖住时
// 不投递进入、补发离开;归属恢复时补发进入(同 Chromium RenderWidgetHostViewCocoa 对被遮挡事件的处理)。
- (BOOL)routeTrackingEvent:(NSEvent*)event;
// responder 属于哪个平台视图;不属于返回 0。
- (int64_t)viewIdForResponder:(NSResponder*)responder;
// 是否持有任何平台视图。
@property(nonatomic, readonly) BOOL hasViews;
// 鼠标派发器(弱引用, MacWindow 持有)。宿主视图在 AppKit 命中测试时据此路由。
@property(nonatomic, weak) MomoMouseEventDispatcher* dispatcher;
// 当前可见平台视图的派发目标, 自上而下(z 序最高在前)。
- (NSArray<id<MomoMouseEventTarget>>*)mouseTargetsTopDown;

@end

#endif  // __OBJC__
