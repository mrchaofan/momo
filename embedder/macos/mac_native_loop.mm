#include "embedder/macos/mac_native_loop.h"

#import <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>
#include <pthread.h>

#include <chrono>
#include <memory>

namespace momo::embedder::macos {

namespace {

dispatch_block_t MakeBlock(Closure task) {
  // block 只能捕获可拷贝对象: 用 shared_ptr 托管仅可移动的 Closure。
  auto shared = std::make_shared<Closure>(std::move(task));
  return ^{
    (*shared)();
  };
}

// 结束 [NSApp run]。
void StopMainLoop() {
  // 模态循环(系统的"重新打开窗口"提示、应用自己的 runModal)里 stop: 只结束模态循环, 主循环照常继续。
  const bool in_modal = NSApp.modalWindow != nil;
  [NSApp stop:nil];
  // stop 只在处理完下一个事件后生效: 投递一个空事件唤醒 run loop。
  NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:0
                                         data1:0
                                         data2:0];
  [NSApp postEvent:event atStart:NO];
  if (in_modal) {
    // 模态循环不在 default mode 下运行: 这个 block 要等它退出、回到外层循环后才执行, 届时再停一次
    // (多层模态则逐层重复)。
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{
      StopMainLoop();
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
  }
}

}  // namespace

void MacNativeLoop::PostTask(Closure task) {
  dispatch_async(dispatch_get_main_queue(), MakeBlock(std::move(task)));
}

void MacNativeLoop::PostDelayedTask(Closure task, TimeDelta delay) {
  const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ns > 0 ? ns : 0), dispatch_get_main_queue(),
                 MakeBlock(std::move(task)));
}

bool MacNativeLoop::RunsTasksOnCurrentThread() const { return pthread_main_np() != 0; }

void MacNativeLoop::Run() { [NSApp run]; }

void MacNativeLoop::Quit() { StopMainLoop(); }

}  // namespace momo::embedder::macos
