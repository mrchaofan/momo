#pragma once

// macOS 的鼠标与其他位置型输入(docs/05 §9 "输入")。仅被 .mm 包含。
//   - MomoClassifyMouseEvent: 事件分类(纯函数)。窗口的 -sendEvent: 按此分流: None 以外的事件全部先交给 momo,
//     不由 AppKit 按几何直接投递给原生视图。
//   - MacMouseInput: 进程内一个(由 MacPlatformIntegration 持有, 注入每个 MacWindow)。持有平台无关的
//     PointerDispatcher, 把 NSEvent 翻译成 PointerInput 交给它;并用进程级本地事件监视器接住按下序列期间
//     不属于任何 momo 窗口的事件(按下所在的窗口已关闭、拖到了别的窗口上)。
//   - 跟踪区工具: 按 AppKit 的规则判断跟踪区是否生效、把移动投递给跟踪区 owner(窗口装饰与平台视图共用)。
//   - 屏幕坐标: 对外统一为主屏左上原点、y 向下的逻辑点(与 Quartz 全局坐标一致)。
// 线程约定: 只在主(platform)线程使用。

#import <Cocoa/Cocoa.h>

#include <cstdint>

#include "base/geometry.h"
#include "base/macros.h"
#include "embedder/common/pointer_dispatcher.h"

typedef NS_ENUM(NSInteger, MomoMouseEventKind) {
  MomoMouseEventKindNone,  // 非鼠标事件(或不按鼠标处理的: 光标更新…)
  MomoMouseEventKindDown,
  MomoMouseEventKindUp,
  MomoMouseEventKindDrag,
  MomoMouseEventKindMove,
  MomoMouseEventKindScroll,
  MomoMouseEventKindEnter,
  MomoMouseEventKindExit,
  // 其他带位置的输入: 触控板捏合/旋转/轻扫/智能缩放、查询、Force Touch 压力、手写板点、手势开始/结束。
  // 与鼠标事件一样先交给 momo, 由框架决定接收者(docs/05 §9 "输入")。
  MomoMouseEventKindGesture,
};

MomoMouseEventKind MomoClassifyMouseEvent(NSEvent* event);

// 跟踪区当前是否生效(按其激活条件, 与 AppKit 相同)。
BOOL MomoTrackingAreaIsActive(NSTrackingArea* area, NSView* view);
// 跟踪区在 view 坐标中的范围(NSTrackingInVisibleRect 时为可见范围)。
NSRect MomoTrackingRect(NSTrackingArea* area, NSView* view);
// 按 AppKit 的跟踪区规则把移动投递给 root 子树(跳过隐藏的视图与 skip 子树)中包含指针、当前生效的
// NSTrackingMouseMoved 跟踪区的 owner(同一 owner 只收一次)。返回是否投递给了任何 owner。
BOOL MomoDeliverMouseMoved(NSView* root, NSEvent* event, NSView* skip);

namespace momo::embedder::macos {

// 主屏顶边(Cocoa 屏幕坐标)。Cocoa 以主屏左下为原点、y 向上;与对外坐标互换只需按它翻转 y。
CGFloat PrimaryScreenTop();
NSPoint FlipScreenY(NSPoint point);
// Cocoa 屏幕矩形 → 对外坐标(主屏左上原点)。
Rect ScreenRectFromCocoa(NSRect rect);

class MacMouseInput {
 public:
  MacMouseInput();
  ~MacMouseInput();

  PointerDispatcher& dispatcher() { return dispatcher_; }

  // momo 窗口(NSWindow)的登记: 序列期间属于登记窗口的事件由该窗口的 -sendEvent: 交给 Dispatch,
  // 其余的由进程级监视器交给 Dispatch(window = nullptr)。
  void AddWindow(NSWindow* window);
  void RemoveWindow(NSWindow* window);

  // 翻译 event 并交给派发器(window: 系统把事件交给的 momo 窗口)。不是指针输入时什么也不做。
  void Dispatch(NSEvent* event, PointerWindow* window);
  // 进程级监视器的判定(测试也直接调用): 按下序列进行中、事件不属于任何登记窗口时交给派发器并返回 true
  // (不再照常派发);否则返回 false。
  bool RouteSequenceEvent(NSEvent* event);

 private:
  bool Translate(NSEvent* event, PointerWindow* window, PointerInput& input);
  uint32_t LogicalButton(NSEvent* event);

  PointerDispatcher dispatcher_;
  NSHashTable<NSWindow*>* windows_;
  id sequence_monitor_ = nil;
  // Control+左键按下视为次键(与 AppKit 弹出上下文菜单的惯例一致), 直到该左键抬起。
  bool control_click_ = false;

  MOMO_DISALLOW_COPY_AND_ASSIGN(MacMouseInput);
};

}  // namespace momo::embedder::macos
