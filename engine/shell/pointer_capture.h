#pragma once

// 跨视图的指针捕获(多视图, docs/02 §2、docs/03 §3)。
// 一个设备在某个视图里按下后, 到抬起(或该视图移除/指针离开)为止, 这个设备的事件只接受来自该视图的;
// 其他视图在此期间发来的同一设备事件(进入/悬停/离开/滚动)一律丢弃。
// 原因: 各窗口独立追踪鼠标, 拖动途中经过别的窗口(如刚拖出的新窗口)时, 那个窗口会按"未按下"上报
// add/hover/remove;框架所有视图共用一套指针状态, 这些事件会被当成正在按下的指针被移动或取消,
// 拖动因此中断。效果相当于 Chromium / Win32 的 SetCapture: 按下所在窗口独占这次按下序列。

#include <cstdint>
#include <map>
#include <optional>

#include "engine/platform/input_types.h"
#include "engine/platform/view.h"

namespace momo::engine {

class PointerCapture {
 public:
  // 过滤来自 view 的指针包: 返回应交给框架的事件(可能为空)。
  platform::PointerDataPacket Filter(platform::ViewId view, const platform::PointerDataPacket& packet);

  // 视图被移除: 释放它持有的捕获。返回需要补发的事件(对每个被释放的设备补一个 remove, 让框架取消
  // 进行中的手势);由调用方经任一仍存在的视图交给框架。
  platform::PointerDataPacket ReleaseView(platform::ViewId view);

  // 显式捕获(view.setCapture()): 把所有进行中的按下序列转交给 view。之后原视图的事件被丢弃,
  // 由 view 的宿主窗口接管并补发一次按下(框架据此在新窗口开始新的手势序列)。返回是否有序列被转交。
  bool TransferTo(platform::ViewId view);

  // 设备当前的捕获视图(测试与诊断用)。
  std::optional<platform::ViewId> captured_by(int32_t device) const;

 private:
  std::map<int32_t, platform::ViewId> captures_;
};

}  // namespace momo::engine
