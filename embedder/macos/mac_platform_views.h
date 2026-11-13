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
// 输入: 这里只提供几何。每一帧的几何(可见矩形、z 序、盖在其上的 momo 内容)与画面在同一个 CATransaction 里
//   生效(applyLayout), 所以 platformViewAtWindowPoint: 回答的就是"用户此刻看到的指针下是什么"——内容视图据此在
//   主线程同步决定原生事件的接收方(PointerDispatcher), 不经 ui 线程往返。投递由窗口的
//   MomoPlatformViewInputRouter 完成。容器与宿主吞掉沿响应链冒泡上来的鼠标事件, 原生视图未处理的事件不会漏给 momo。
// 线程约定: 只在主(platform)线程使用;MacMetalSurface 在主线程呈现合成帧时调用 applyLayout。

#include <cstdint>
#include <vector>

#include "engine/platform/platform_view.h"

#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>


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
// ---- 几何查询(主线程同步)----
// 窗口坐标处最上层的平台视图: 在其可见区域内、且没有被之上绘制的 momo 内容盖住(occluders);否则 0(归 momo)。
// 依据最近一次已呈现的帧。
- (int64_t)platformViewAtWindowPoint:(NSPoint)point;
// 每次几何变化(applyLayout / hideAll / removeViewId)之后在主线程回调: 指针不动而画面变化时(菜单弹出盖住网页、
// 网页出现在指针下), 窗口据此重新判定指针下的接收方。
@property(nonatomic, copy) void (^geometryChanged)(void);
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
// 指针此刻归本窗口的谁(PointerDispatcher): 0 = momo 内容, 其他 = 平台视图 id, -1 = 宿主不掌握指针
// (指针不在本窗口、拖放会话等)。AppKit 的几何命中测试据此回答(见实现处说明)。
@property(nonatomic, copy) int64_t (^pointerOwner)(void);
@end

#endif  // __OBJC__
