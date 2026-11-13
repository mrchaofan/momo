#pragma once

// PointerDispatcher: 宿主里鼠标与其他位置型输入的唯一决策者(docs/05 §9 "输入")。进程内一个, 平台无关。
//
// 分工(中介者): 平台层把原生事件翻译成 PointerInput 交给 Dispatch, 派发器决定接收方, 再经 PointerWindow
// 执行——交给某个窗口的 momo 内容(指针包), 或把原生事件交给某个平台视图。接收方的状态只在这里有一份:
// 按下序列、悬停目标、滚动/手势锁定、momo 是否已接入指针。平台层不做任何路由决定。
//
// 规则:
//   - 按下序列(首个按下到最后一个抬起)的接收方在按下时决定并固定: 按下处是平台视图就交给它, 否则交给
//     momo 内容。序列期间按键、拖动、抬起、压力都交给它(不论系统把事件交给哪个窗口, 坐标按屏幕位置换算);
//     其他窗口的悬停、进出、滚动、手势丢弃。原生接收方默认如此(系统级捕获), momo 也一样。
//   - Capture: momo 序列改交另一个窗口(拖出标签成窗、并入另一个窗口), 不合成事件——框架的逻辑接收方不变。
//   - 没有序列时: 移动按指针下的接收方更新悬停(原生视图的进入/离开、momo 的接入/离开);滚动与触控板
//     手势在开始时锁定接收方直到结束。
//   - 过期: 系统层已没有按键按住而宿主没见到抬起(被原生菜单、系统窗口拖动等嵌套事件循环吞掉)——
//     补报抬起结束旧序列。
//   - 指针包语义: kRemove 只表示离开(momo 不再掌握指针), kCancel 表示序列被取消(接收窗口关闭)。
//     同一序列的事件带相同的 sequence_id(> 0), 窗口能力(startDrag、capturePointer)据此校验。
// 命中依据平台层的 HitTest(按已呈现的帧, 即用户此刻看到的画面), 不经 ui 线程往返。
// 坐标: 派发器只用屏幕坐标(逻辑点, 主屏左上原点);换算到窗口的物理像素由窗口完成。
// 线程约定: 仅在 platform 线程使用。

#include <cstdint>
#include <optional>

#include "base/geometry.h"
#include "base/macros.h"
#include "embedder/common/engine_platform.h"
#include "engine/platform/input_types.h"

namespace momo::embedder {

class PointerWindow;

// 一次原生指针输入(平台层翻译)。
struct PointerInput {
  enum class Type : uint8_t {
    kDown,
    kUp,
    kMove,      // 移动或拖动(是否在序列内由派发器判断)
    kScroll,
    kGesture,   // 触控板捏合/旋转/轻扫等(按自身阶段锁定接收方)
    kPressure,  // 属于按下序列的输入: Force Touch 压力、按住时的手写板点
    kLeave,     // 指针离开窗口
  };
  Type type = Type::kMove;
  // 系统把事件交给的 momo 窗口;nullptr = 不属于任何 momo 窗口(序列期间的窗口外拖动等)。
  PointerWindow* window = nullptr;
  // 已填: timestamp_us、kind、screen_x/y、滚动位移(逻辑点)与手势字段。
  // 由派发器填: phase、buttons、sequence_id;由窗口填: physical_x/y(并把滚动位移换算为物理像素)。
  platform::PointerData data;
  uint32_t button = 0;       // kDown / kUp: 变化的逻辑按键位
  bool others_held = false;  // 系统层除本事件的按键外是否还有按键按住(过期判定)
  void* native = nullptr;    // 原生事件(macOS: NSEvent*), 派发器不解读, 交回平台层投递
};

// 派发器眼里的一个窗口(平台窗口实现)。
class PointerWindow {
 public:
  // screen 处本窗口的接收方(按已呈现的帧): nullopt = 不在本窗口内容区内或被别的窗口遮住;
  // 0 = momo 内容;其他 = 平台视图 id。
  virtual std::optional<int64_t> HitTest(Point screen) const = 0;
  // 交给 momo 内容: 包里的坐标是屏幕坐标, 窗口换算后上报。input 为引起它的输入(合成事件的 native 为空)。
  virtual void SendToMomo(platform::PointerDataPacket packet, const PointerInput& input) = 0;
  // 把 input 的原生事件交给平台视图。phase 与 momo 的语义相同: kDown 序列开始、kMove 序列内、
  // kUp 序列结束、kHover 悬停移动、kScroll / kGesture。
  virtual void SendToPlatformView(int64_t view, const PointerInput& input,
                                  platform::PointerPhase phase) = 0;
  // 指针所在的平台视图变化(0 = 不在平台视图上): 原生视图的进入/离开、光标归属。input 为引起变化的输入。
  virtual void SetHoveredPlatformView(int64_t view, const PointerInput& input) = 0;
  // 移动循环: 把内容区左上角移到屏幕位置 top_left。
  virtual void MoveTo(Point top_left) = 0;

 protected:
  ~PointerWindow() = default;
};

class PointerDispatcher {
 public:
  PointerDispatcher() = default;

  void Dispatch(const PointerInput& input);

  // momo/window capturePointer: 把进行中的 momo 序列改交 window。sequence_id 不匹配、没有序列、
  // 序列归平台视图时返回 false。
  bool Capture(PointerWindow* window, int64_t sequence_id);
  // window 的 momo 内容是否是序列 sequence_id 的接收方(startDrag 校验)。
  bool IsMomoSequence(PointerWindow* window, int64_t sequence_id) const;
  // 序列在系统层已结束(系统窗口拖动返回时按键已松开): 补报抬起。
  void EndSequence(PointerWindow* window, Point screen, int64_t timestamp_us);
  // 移动循环(momo/window startMoveLoop): 直到序列结束, 每个拖动先把 window 移到 指针 − grab。
  // window 不是进行中 momo 序列的接收方时返回 false。
  bool StartMoveLoop(PointerWindow* window, Point grab);
  void EndMoveLoop(PointerWindow* window);

  // 窗口即将关闭: 它是 momo 序列的接收方时上报取消(kCancel), 之后到松开为止的事件丢弃。
  void ReleaseCapture(PointerWindow* window);
  // 窗口已销毁: 忘掉它(不再发送任何事件)。
  void RemoveWindow(PointerWindow* window);
  // window 的平台视图几何变化(新的一帧已呈现): 指针不动、没有序列时重新判定悬停。
  void GeometryChanged(PointerWindow* window);

  // 指针此刻归 window 的谁: 0 = momo 内容, 其他 = 平台视图 id;nullopt = 不在这个窗口(或序列归别的窗口)。
  // 序列期间是序列的接收方。光标归属、AppKit 的几何命中据此回答。
  std::optional<int64_t> PointerOwner(const PointerWindow* window) const;
  bool has_sequence() const { return sequence_.id != 0; }

 private:
  struct Target {
    PointerWindow* window = nullptr;  // nullptr = 无
    int64_t view = 0;                 // 0 = momo 内容
    friend bool operator==(const Target&, const Target&) = default;
  };
  struct Sequence {
    int64_t id = 0;   // 0 = 没有序列
    Target target;    // target.window == nullptr: 接收窗口已关闭, 丢弃到松开
    uint32_t buttons = 0;
    std::optional<Point> move_grab;
  };

  void BeginSequence(const PointerInput& input);
  void DispatchInSequence(const PointerInput& input);
  void SendToSequence(const PointerInput& input, platform::PointerPhase phase);
  void FinishSequence(const PointerInput& input);
  void Hover(const PointerInput& input);
  void LockAndSend(Target& lock, const PointerInput& input, platform::PointerPhase phase);
  void SetHover(Target target, const PointerInput& input);
  Target HitTarget(PointerWindow* window, const PointerInput& input) const;
  void SendToMomo(PointerWindow* window, const PointerInput& input, platform::PointerPhase phase);
  void Send(const Target& target, const PointerInput& input, platform::PointerPhase phase);
  void RemoveFromMomo(const PointerInput& input);
  PointerInput Synthesized(Point screen, int64_t timestamp_us) const;

  Sequence sequence_;
  int64_t next_sequence_id_ = 1;
  Target hover_;                // 没有序列时指针所在的接收方
  PointerWindow* momo_ = nullptr;  // momo 已接入指针(收到 add、未收到 remove)的窗口
  Target scroll_lock_;
  Target gesture_lock_;
  PointerInput last_;           // 最近的输入(合成事件的位置与时间)

  MOMO_DISALLOW_COPY_AND_ASSIGN(PointerDispatcher);
};

}  // namespace momo::embedder
