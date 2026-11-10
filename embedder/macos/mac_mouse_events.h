#pragma once

// 鼠标与其他位置型输入的分类(纯函数, 便于测试)。MomoWindow -sendEvent: 与内容视图按此分流:
// 分类为 None 以外的事件全部先交给 momo, 不由 AppKit 按几何直接投递给原生视图。

#import <Cocoa/Cocoa.h>

typedef NS_ENUM(NSInteger, MomoMouseEventKind) {
  MomoMouseEventKindNone,  // 非鼠标事件(或不按鼠标处理的: 手势、光标更新…)
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
