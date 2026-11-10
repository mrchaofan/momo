#pragma once

// 鼠标事件派发器(docs/05 §9 "输入"): 平台视图(原生 NSView)与 momo 画面交替叠放时, 保证每个鼠标事件
// 只有一个接收者。设计参照 Chromium views(View::OnMousePressed 返回 bool 表示"已处理", 按下的目标
// 捕获后续拖拽/抬起)与 Flutter(PointerRouter: 按下时命中测试一次, 之后整个指针序列路由给同一目标)。
//
// 目标(MomoMouseEventTarget)由上到下排列(z 序), 派发器依次调用 momoHandleMouseEvent:captured:,
// 第一个返回 YES 的目标"消费"这次事件, 不再交给任何其他目标:
//   - 按下: 命中测试选出目标并捕获;捕获期间(任一按钮仍按下)的拖拽/抬起/其他按钮都直接交给它。
//   - 移动(无按钮): 每次命中测试;悬停目标变化时先通知旧目标 momoMouseLeft:(momo 据此发指针 remove),
//     再通知新目标 momoMouseEntered:。
//   - 滚动: 触控板手势(phase Began … 惯性结束)锁定首个消费者;普通滚轮每次命中测试。
//   - 离开窗口: 通知悬停目标 momoMouseLeft:。
// 目标有两种投递方式: momo 内容视图在 momoHandleMouseEvent 里直接处理;平台视图容器只"认领"
// (momoDeliversViaAppKit = YES), 由 MomoWindow 调 -[NSWindow sendEvent:] 让 AppKit 投递给原生视图,
// AppKit 命中测试期间(routing)宿主视图按 routedTarget 返回该原生视图, 其他目标一律不可命中。
// 线程约定: 只在主线程使用。

#import <Cocoa/Cocoa.h>

@protocol MomoMouseEventTarget <NSObject>
// 处理(或认领)一个鼠标事件。captured = YES 表示本目标已捕获该指针序列/滚动手势, 必须处理,
// 返回值被忽略;否则返回 YES 表示已消费(事件位于本目标可接收的区域内)。
- (BOOL)momoHandleMouseEvent:(NSEvent*)event captured:(BOOL)captured;
@optional
// 悬停离开本目标(移到其他目标上, 或离开窗口)。
- (void)momoMouseLeft:(NSEvent*)event;
// 悬停移到本目标上(在旧目标的 momoMouseLeft: 之后)。悬停与捕获是"指针归属"的唯一依据:
// 平台视图据此决定原生视图何时认为指针在它里面(见 mac_platform_views.h)。
- (void)momoMouseEntered:(NSEvent*)event;
// YES: 事件由 AppKit 原生通路投递(平台视图);NO(默认): 目标已在 momoHandleMouseEvent 中处理。
- (BOOL)momoDeliversViaAppKit;
@end

// 事件在派发器中的分类(纯函数, 便于测试)。
typedef NS_ENUM(NSInteger, MomoMouseEventKind) {
  MomoMouseEventKindNone,  // 非鼠标事件(或派发器不处理的: 手势、光标更新…)
  MomoMouseEventKindDown,
  MomoMouseEventKindUp,
  MomoMouseEventKindDrag,
  MomoMouseEventKindMove,
  MomoMouseEventKindScroll,
  MomoMouseEventKindEnter,
  MomoMouseEventKindExit,
};
MomoMouseEventKind MomoClassifyMouseEvent(NSEvent* event);

@interface MomoMouseEventDispatcher : NSObject

// 当前的目标列表, 自上而下(z 序最高在前)。每次派发时调用。
@property(nonatomic, copy) NSArray<id<MomoMouseEventTarget>>* (^targetsProvider)(void);
// 当前实际按下的鼠标按钮位图(缺省 +[NSEvent pressedMouseButtons];测试可替换)。
// 用于识别"卡住"的捕获: 原生菜单等嵌套事件循环会吞掉抬起事件, 它们不经过本派发器。
@property(nonatomic, copy) NSUInteger (^pressedButtonsProvider)(void);

// 派发一个鼠标事件, 返回消费它的目标(nil = 没有目标消费)。
- (id<MomoMouseEventTarget>)dispatchEvent:(NSEvent*)event;

// AppKit 投递期间(MomoWindow 调用 super sendEvent 前后)设置: 宿主视图的 hitTest 据此只返回该目标。
- (void)beginRoutingToTarget:(id<MomoMouseEventTarget>)target;
- (void)endRouting;
@property(nonatomic, readonly) BOOL routing;
@property(nonatomic, readonly, weak) id<MomoMouseEventTarget> routedTarget;

// 诊断/测试。
@property(nonatomic, readonly, weak) id<MomoMouseEventTarget> captureTarget;
@property(nonatomic, readonly, weak) id<MomoMouseEventTarget> hoverTarget;
@property(nonatomic, readonly, weak) id<MomoMouseEventTarget> scrollTarget;
@property(nonatomic, readonly) NSUInteger pressedButtons;

// 目标集合变化(平台视图移除等)时调用: 清除对已消失目标的捕获/悬停/滚动锁定。
- (void)forgetTarget:(id<MomoMouseEventTarget>)target;

@end
