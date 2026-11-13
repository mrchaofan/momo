#pragma once

// 平台无关输入事件(docs/03 §3)。坐标统一为物理像素。

#include <cstdint>
#include <string>
#include <vector>

namespace momo::engine::platform {

enum class PointerDeviceKind : uint8_t { kMouse, kTrackpad, kTouch, kStylus };
// kGesture: 带位置、但不是按钮/移动/滚动的输入(触控板捏合与旋转、轻扫、Force Touch 压力…)。
// 全部位置型输入都先交给框架, 由框架决定接收者(docs/05 §9 "输入"), 不由宿主按几何直接投递给原生视图。
// kRemove 只表示指针离开(框架不再掌握它的位置);kCancel 表示进行中的按下序列被取消(如接收窗口关闭)。
enum class PointerPhase : uint8_t { kAdd, kDown, kMove, kUp, kRemove, kHover, kScroll, kGesture, kCancel };
// 触控板手势的阶段(kScroll / kGesture 事件)。框架据此在整个手势(含惯性)内锁定接收者:
// kStart 重新命中测试并锁定, kUpdate 沿用锁定, kEnd 沿用后解除。没有阶段的(普通滚轮)为 kNone, 每次命中测试。
enum class SignalPhase : uint8_t { kNone, kStart, kUpdate, kEnd };
// kGesture 事件的种类。
enum class PointerGestureKind : uint8_t {
  kNone,
  kMagnify,       // 双指捏合: gesture_value = 本次缩放增量(1 + value 为比例)
  kRotate,        // 双指旋转: gesture_value = 本次旋转角度(度, 逆时针为正)
  kSmartMagnify,  // 双指轻点两下(智能缩放)
  kSwipe,         // 三指轻扫: scroll_delta_x/y = 方向(-1/0/1)
  kQuickLook,     // 查询(三指轻点 / 用力点按)
  kPressure,      // Force Touch 压力(按下序列内): gesture_value = 压力 0..1, gesture_stage = 阶段 0..2
  kTabletPoint,   // 手写板点(位置与压力)
  kBegin,         // 触控板手势开始/结束(未细分种类的手势边界;signal_phase = kStart / kEnd)
  kEnd,
};

struct PointerData {
  int64_t timestamp_us = 0;
  PointerDeviceKind kind = PointerDeviceKind::kMouse;
  PointerPhase phase = PointerPhase::kHover;
  int32_t device = 0;
  double physical_x = 0;
  double physical_y = 0;
  uint32_t buttons = 0;  // 位图: bit0 主键, bit1 次键, bit2 中键
  double scroll_delta_x = 0;
  double scroll_delta_y = 0;
  bool momentum = false;  // Mac 惯性滚动阶段
  // 指针的屏幕位置(逻辑点, 主屏左上原点、y 向下;与窗口 screen_frame 同一坐标系)。
  // 跨窗口交互(拖出标签成窗)用它计算, 不受窗口自身移动的影响。
  double screen_x = 0;
  double screen_y = 0;
  SignalPhase signal_phase = SignalPhase::kNone;  // kScroll / kGesture
  PointerGestureKind gesture_kind = PointerGestureKind::kNone;  // kGesture
  double gesture_value = 0;
  int32_t gesture_stage = 0;
  // 按下序列的 id(宿主分配, 进程内递增, > 0;不在序列内的事件为 0)。同一序列的事件相同;
  // 框架调用依赖序列的窗口能力(momo/window startDrag、capturePointer)时回送, 宿主据此校验。
  int64_t sequence_id = 0;
};

using PointerDataPacket = std::vector<PointerData>;

enum class KeyEventType : uint8_t { kDown, kUp, kRepeat };

enum KeyModifiers : uint32_t {
  kModifierNone = 0,
  kModifierShift = 1 << 0,
  kModifierControl = 1 << 1,
  kModifierAlt = 1 << 2,
  kModifierMeta = 1 << 3,
  kModifierCapsLock = 1 << 4,
};

struct KeyData {
  int64_t timestamp_us = 0;
  KeyEventType type = KeyEventType::kDown;
  uint64_t physical_key = 0;  // USB HID usage
  uint64_t logical_key = 0;
  std::string character;      // UTF-8, 可为空
  uint32_t modifiers = kModifierNone;
};

}  // namespace momo::engine::platform
