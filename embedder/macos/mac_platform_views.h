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
// 输入: 不在这里处理。谁接收由框架决定, 投递由窗口的 MomoPlatformViewInputRouter 完成
//   (mac_platform_view_input.h);这里只提供几何查询。容器与宿主吞掉沿响应链冒泡上来的鼠标事件,
//   原生视图未处理的事件不会漏给 momo。
// 线程约定: 只在主(platform)线程使用;MacMetalSurface 在主线程呈现合成帧时调用 applyLayout。

#include <cstdint>
#include <vector>

#include "engine/platform/platform_view.h"

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

@class MomoPlatformViewInputRouter;

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
// ---- 供输入路由器的几何查询 ----
// 平台视图 viewId 当前可见且挂在本窗口时的原生视图;否则 nil(隐藏、已移除或已被其他窗口接管)。
- (NSView*)hostedNativeForPlatformView:(int64_t)viewId;
// 窗口坐标处在平台视图 viewId 的原生视图子树中命中的视图(不考虑遮挡: 接收者已由框架决定)。
- (NSView*)nativeHitTestForPlatformView:(int64_t)viewId windowPoint:(NSPoint)point;
// 跟踪区属于哪个平台视图的原生视图子树;不属于返回 0。
- (int64_t)platformViewOwningTrackingArea:(NSTrackingArea*)area;
// responder 属于哪个平台视图;不属于返回 0。
- (int64_t)viewIdForResponder:(NSResponder*)responder;
// 是否持有任何平台视图。
@property(nonatomic, readonly) BOOL hasViews;
// 窗口的输入路由(弱引用, MacWindow 持有)。AppKit 的几何命中测试据其指针归属回答(见实现处说明)。
@property(nonatomic, weak) MomoPlatformViewInputRouter* inputRouter;
@end

#endif  // __OBJC__
