#include "embedder/macos/mac_vsync_displaylink.h"

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>

#include <chrono>
#include <utility>

#include "base/logging.h"
#include "embedder/macos/mac_unfair_lock.h"

namespace momo::embedder::macos {

namespace {

// mach host time(CVTimeStamp.hostTime)→ momo::TimePoint(steady_clock), 以"现在"为锚点换算。
TimePoint HostTimeToTimePoint(uint64_t host_time, uint64_t now_host, TimePoint now) {
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return info;
  }();
  const double delta_ticks = static_cast<double>(host_time) - static_cast<double>(now_host);
  const double delta_ns = delta_ticks * timebase.numer / timebase.denom;
  return now + std::chrono::duration_cast<TimeDelta>(std::chrono::duration<double, std::nano>(delta_ns));
}

CGDirectDisplayID DisplayIdForWindow(NSWindow* window) {
  NSScreen* screen = window.screen ?: [NSScreen mainScreen];
  NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
  return number ? static_cast<CGDirectDisplayID>(number.unsignedIntValue) : CGMainDisplayID();
}

}  // namespace

// platform 线程与 CVDisplayLink 线程共享的状态(全部字段受 lock 保护)。
// CVDisplayLink 线程只写入最新帧时间并投递通知, 从不调用 callback(回调在调用方线程执行)。
struct MacVsyncDisplayLink::Shared {
  UnfairLock lock;
  bool destroyed = false;     // 析构已开始: 回调线程不再投递
  bool tick_posted = false;   // 已向 platform 线程投递 OnTick 且尚未处理(合并多余的 tick)
  TimePoint frame_start;      // 最近一个 tick 的帧时间
  TimePoint frame_target;
  WeakPtr<MacVsyncDisplayLink> owner;

  static CVReturn OnDisplayLink(CVDisplayLinkRef, const CVTimeStamp* now,
                                const CVTimeStamp* output_time, CVOptionFlags, CVOptionFlags*,
                                void* context) {
    auto* shared = static_cast<Shared*>(context);
    const TimePoint now_point = Clock::now();
    const uint64_t now_host = mach_absolute_time();
    const TimePoint start = HostTimeToTimePoint(now->hostTime, now_host, now_point);
    TimePoint target = HostTimeToTimePoint(output_time->hostTime, now_host, now_point);
    if (target <= start) {
      target = start + std::chrono::milliseconds(16);
    }
    UnfairLockScope scope(shared->lock);
    if (shared->destroyed) {
      return kCVReturnSuccess;
    }
    shared->frame_start = start;
    shared->frame_target = target;
    if (!shared->tick_posted) {
      shared->tick_posted = true;
      PostToObject(shared->owner, [](MacVsyncDisplayLink& self) { self.OnTick(); });
    }
    return kCVReturnSuccess;
  }
};

StatusOr<ObjectPtr<platform::VsyncSource>> MacVsyncDisplayLink::Create(NSWindow* window) {
  CVDisplayLinkRef link = nullptr;
  if (CVDisplayLinkCreateWithActiveCGDisplays(&link) != kCVReturnSuccess || link == nullptr) {
    return UnavailableError("CVDisplayLinkCreateWithActiveCGDisplays failed");
  }
  ObjectPtr<MacVsyncDisplayLink> source = MakeObject<MacVsyncDisplayLink>(link, window);
  return ObjectPtr<platform::VsyncSource>(source.release());
}

MacVsyncDisplayLink::MacVsyncDisplayLink(CVDisplayLinkRef link, NSWindow* window)
    : link_(link), shared_(std::make_unique<Shared>()), window_(window) {
  shared_->owner = WeakPtr<MacVsyncDisplayLink>(this);
  CVDisplayLinkSetOutputCallback(link_, &Shared::OnDisplayLink, shared_.get());
  UpdateDisplay();
  if (window != nil) {
    // 观察者块在主线程(= platform 线程)同步执行;析构时移除, 因此捕获 this 安全。
    MacVsyncDisplayLink* self = this;
    screen_observer_ = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowDidChangeScreenNotification
                    object:window
                     queue:nil
                usingBlock:^(NSNotification*) {
                  self->UpdateDisplay();
                }];
  }
}

MacVsyncDisplayLink::~MacVsyncDisplayLink() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  if (screen_observer_ != nil) {
    [[NSNotificationCenter defaultCenter] removeObserver:screen_observer_];
    screen_observer_ = nil;
  }
  {
    UnfairLockScope scope(shared_->lock);
    shared_->destroyed = true;  // 之后的 tick 什么也不做
  }
  // 非回调线程调用 Stop 会等待在途回调返回;此后 link 不再引用 shared_。
  CVDisplayLinkStop(link_);
  CVDisplayLinkRelease(link_);
  link_ = nullptr;
  { UnfairLockScope scope(shared_->lock); }  // 与可能仍持锁的回调汇合(防御)
}

void MacVsyncDisplayLink::RequestFrameCallback(Callback callback) {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  callback_ = std::move(callback);  // 一次性: 新请求覆盖未触发的旧请求
  if (!running_) {
    if (CVDisplayLinkStart(link_) == kCVReturnSuccess) {
      running_ = true;
    } else {
      MOMO_LOG(ERROR) << "CVDisplayLinkStart failed";
    }
  }
}

void MacVsyncDisplayLink::OnTick() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  TimePoint start;
  TimePoint target;
  {
    UnfairLockScope scope(shared_->lock);
    shared_->tick_posted = false;
    start = shared_->frame_start;
    target = shared_->frame_target;
  }
  if (!callback_) {
    // 空闲 tick: 没有请求就停掉 display link。请求与停止都在本线程, 无竞态。
    if (running_) {
      CVDisplayLinkStop(link_);
      running_ = false;
    }
    return;
  }
  // 先取走再调用: callback 里可以再次 RequestFrameCallback。
  Callback callback = std::move(callback_);
  callback_ = nullptr;
  callback(start, target);
}

void MacVsyncDisplayLink::UpdateDisplay() {
  MOMO_DCHECK_ON_AFFINITY_THREAD();
  NSWindow* window = window_;
  const CGDirectDisplayID display = window ? DisplayIdForWindow(window) : CGMainDisplayID();
  if (CVDisplayLinkGetCurrentCGDisplay(link_) != display) {
    CVDisplayLinkSetCurrentCGDisplay(link_, display);
    MOMO_LOG(VERBOSE) << "VSync display link switched to display " << display;
  }
}

}  // namespace momo::embedder::macos
