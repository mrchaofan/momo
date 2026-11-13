// PointerDispatcher: 指针输入的接收方决策(按下序列、悬停、滚动锁定、捕获、过期)。

#include "embedder/common/pointer_dispatcher.h"

#include <map>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace momo::embedder {
namespace {

using platform::PointerPhase;
using platform::SignalPhase;
using Type = PointerInput::Type;

std::string PhaseName(PointerPhase phase) {
  switch (phase) {
    case PointerPhase::kAdd: return "add";
    case PointerPhase::kDown: return "down";
    case PointerPhase::kMove: return "move";
    case PointerPhase::kUp: return "up";
    case PointerPhase::kRemove: return "remove";
    case PointerPhase::kHover: return "hover";
    case PointerPhase::kScroll: return "scroll";
    case PointerPhase::kGesture: return "gesture";
    case PointerPhase::kCancel: return "cancel";
  }
  return "?";
}

bool Contains(const Rect& r, Point p) { return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height; }

// 屏幕上的一个窗口: 内容区 frame, 其中若干平台视图(矩形, 后加入的在上)。
class FakeWindow final : public PointerWindow {
 public:
  explicit FakeWindow(Rect frame) : frame(frame) {}

  std::optional<int64_t> HitTest(Point p) const override {
    if (covered || !Contains(frame, p)) return std::nullopt;
    for (auto it = views.rbegin(); it != views.rend(); ++it) {
      if (Contains(it->second, p)) return it->first;
    }
    return 0;
  }
  void SendToMomo(platform::PointerDataPacket packet, const PointerInput&) override {
    for (const platform::PointerData& d : packet) {
      momo.push_back(PhaseName(d.phase));
      last_momo = d;
    }
  }
  void SendToPlatformView(int64_t view, const PointerInput&, PointerPhase phase) override {
    native.push_back(std::to_string(view) + ":" + PhaseName(phase));
  }
  void SetHoveredPlatformView(int64_t view, const PointerInput&) override {
    native.push_back("hovered " + std::to_string(view));
  }
  void MoveTo(Point top_left) override {
    frame.x = top_left.x;
    frame.y = top_left.y;
  }

  // 取走记录(空格分隔)。
  std::string TakeMomo() { return Take(momo); }
  std::string TakeNative() { return Take(native); }

  Rect frame;
  std::map<int64_t, Rect> views;
  bool covered = false;
  std::vector<std::string> momo;
  std::vector<std::string> native;
  platform::PointerData last_momo;

 private:
  static std::string Take(std::vector<std::string>& log) {
    std::string out;
    for (const std::string& s : log) out += (out.empty() ? "" : " ") + s;
    log.clear();
    return out;
  }
};

class PointerDispatcherTest : public ::testing::Test {
 protected:
  PointerDispatcherTest() : w1_({0, 0, 400, 300}), w2_({500, 0, 400, 300}) {
    w1_.views[7] = Rect{100, 100, 100, 100};
  }

  PointerInput Input(Type type, PointerWindow* window, double x, double y, bool held = false) {
    PointerInput input;
    input.type = type;
    input.window = window;
    input.data.screen_x = x;
    input.data.screen_y = y;
    input.data.timestamp_us = ++time_;
    input.others_held = held;
    if (type == Type::kDown || type == Type::kUp) input.button = 1;
    return input;
  }
  void Move(PointerWindow* w, double x, double y, bool held = false) { d_.Dispatch(Input(Type::kMove, w, x, y, held)); }
  void Down(PointerWindow* w, double x, double y) { d_.Dispatch(Input(Type::kDown, w, x, y)); }
  void Up(PointerWindow* w, double x, double y) { d_.Dispatch(Input(Type::kUp, w, x, y)); }
  void Scroll(PointerWindow* w, double x, double y, SignalPhase phase) {
    PointerInput input = Input(Type::kScroll, w, x, y);
    input.data.signal_phase = phase;
    d_.Dispatch(input);
  }

  PointerDispatcher d_;
  FakeWindow w1_;
  FakeWindow w2_;
  int64_t time_ = 0;
};

// 悬停: momo 内容 ↔ 平台视图之间移动时, momo 收到接入/离开, 原生视图收到进入/离开与移动。
TEST_F(PointerDispatcherTest, HoverMovesBetweenMomoAndPlatformViews) {
  Move(&w1_, 10, 10);
  EXPECT_EQ(w1_.TakeMomo(), "add hover");
  Move(&w1_, 150, 150);
  EXPECT_EQ(w1_.TakeMomo(), "remove");
  EXPECT_EQ(w1_.TakeNative(), "hovered 7 7:hover");
  EXPECT_EQ(d_.PointerOwner(&w1_), 7);
  EXPECT_EQ(d_.PointerOwner(&w2_), std::nullopt);
  Move(&w1_, 20, 20);
  EXPECT_EQ(w1_.TakeNative(), "hovered 0");
  EXPECT_EQ(w1_.TakeMomo(), "add hover");
  // 离开窗口;移到另一个窗口。
  d_.Dispatch(Input(Type::kLeave, &w1_, 450, 20));
  EXPECT_EQ(w1_.TakeMomo(), "remove");
  Move(&w2_, 550, 20);
  EXPECT_EQ(w2_.TakeMomo(), "add hover");
  EXPECT_EQ(w1_.TakeMomo(), "");
}

// momo 的按下序列: 接收方固定到松开——拖过平台视图、拖到别的窗口, 事件都交给按下处的 momo 内容;
// 别的窗口的滚动丢弃。同一序列带同一个 id。
TEST_F(PointerDispatcherTest, MomoSequenceKeepsItsReceiver) {
  Move(&w1_, 10, 10);
  Down(&w1_, 10, 10);
  EXPECT_EQ(w1_.TakeMomo(), "add hover down");
  const int64_t id = w1_.last_momo.sequence_id;
  EXPECT_GT(id, 0);
  EXPECT_EQ(w1_.last_momo.buttons, 1u);
  Move(&w1_, 150, 150, true);  // 拖过平台视图
  Move(&w2_, 600, 50, true);   // 系统把事件交给了别的窗口
  EXPECT_EQ(w1_.TakeMomo(), "move move");
  EXPECT_EQ(w1_.last_momo.screen_x, 600);
  EXPECT_EQ(w1_.last_momo.sequence_id, id);
  Scroll(&w2_, 600, 50, SignalPhase::kNone);
  EXPECT_EQ(w2_.TakeMomo(), "");
  EXPECT_EQ(w1_.TakeNative(), "");
  Up(&w2_, 600, 50);
  EXPECT_EQ(w1_.TakeMomo(), "up remove") << "released outside the window: the pointer left";
  EXPECT_EQ(w1_.last_momo.sequence_id, 0);
  Move(&w1_, 10, 10);
  Down(&w1_, 10, 10);
  EXPECT_EQ(w1_.TakeMomo(), "add hover down");
  EXPECT_GT(w1_.last_momo.sequence_id, id);
}

// 平台视图的按下序列: momo 看不到(指针离开了 momo);拖出视图也交给它;松开后按位置重新判定悬停。
TEST_F(PointerDispatcherTest, NativeSequenceIsCapturedByThePlatformView) {
  Move(&w1_, 10, 10);
  w1_.TakeMomo();
  Down(&w1_, 150, 150);
  EXPECT_EQ(w1_.TakeMomo(), "remove");
  EXPECT_EQ(w1_.TakeNative(), "hovered 7 7:down");
  Move(&w1_, 20, 20, true);
  EXPECT_EQ(w1_.TakeNative(), "7:move");
  EXPECT_EQ(w1_.TakeMomo(), "");
  EXPECT_FALSE(d_.Capture(&w2_, 1)) << "native sequences cannot be captured";
  Up(&w1_, 20, 20);
  EXPECT_EQ(w1_.TakeNative(), "7:up hovered 0");
  EXPECT_EQ(w1_.TakeMomo(), "add hover") << "back on momo content";
}

// 过期: 抬起被嵌套事件循环吞掉(系统层已无按键)——补报抬起, 新事件照常。
TEST_F(PointerDispatcherTest, StaleSequenceIsFinishedWithAnUp) {
  Move(&w1_, 10, 10);
  Down(&w1_, 10, 10);
  w1_.TakeMomo();
  Move(&w1_, 30, 30, /*held=*/false);
  EXPECT_EQ(w1_.TakeMomo(), "up hover");
  EXPECT_FALSE(d_.has_sequence());
  Down(&w1_, 30, 30);
  Down(&w1_, 30, 30);  // 又一个按下, 系统层没有别的按键: 前一个序列过期
  EXPECT_EQ(w1_.TakeMomo(), "down up down");
}

// 捕获: 序列 id 匹配才改交;改交不合成事件, 之后的事件交给新窗口, 原窗口不再收到。
TEST_F(PointerDispatcherTest, CaptureMovesTheSequenceToAnotherWindow) {
  Move(&w1_, 10, 10);
  Down(&w1_, 10, 10);
  const int64_t id = w1_.last_momo.sequence_id;
  w1_.TakeMomo();
  EXPECT_FALSE(d_.Capture(&w2_, id + 1));
  EXPECT_TRUE(d_.Capture(&w2_, id));
  EXPECT_TRUE(d_.Capture(&w2_, id));
  EXPECT_TRUE(d_.IsMomoSequence(&w2_, id));
  EXPECT_FALSE(d_.IsMomoSequence(&w1_, id));
  EXPECT_EQ(w1_.TakeMomo(), "");
  EXPECT_EQ(w2_.TakeMomo(), "");
  Move(&w1_, 520, 20, true);
  EXPECT_EQ(w2_.TakeMomo(), "move");
  EXPECT_EQ(w2_.last_momo.buttons, 1u);
  // 原窗口关闭: 它已不是接收方, 什么也不发生。
  d_.ReleaseCapture(&w1_);
  d_.RemoveWindow(&w1_);
  Up(nullptr, 520, 20);
  EXPECT_EQ(w2_.TakeMomo(), "up");
  EXPECT_EQ(w1_.TakeMomo(), "");
}

// 接收窗口关闭: momo 收到取消, 之后到松开为止的事件丢弃。
TEST_F(PointerDispatcherTest, ClosingTheReceiverCancelsTheSequence) {
  Move(&w1_, 10, 10);
  Down(&w1_, 10, 10);
  w1_.TakeMomo();
  d_.ReleaseCapture(&w1_);
  EXPECT_EQ(w1_.TakeMomo(), "cancel");
  d_.RemoveWindow(&w1_);
  Move(&w2_, 550, 20, true);
  Up(&w2_, 550, 20);
  EXPECT_EQ(w2_.TakeMomo(), "");
  EXPECT_FALSE(d_.has_sequence());
  Move(&w2_, 560, 20);
  EXPECT_EQ(w2_.TakeMomo(), "add hover");
}

// 滚动在开始时锁定接收方直到结束;没有阶段的滚动每次命中。
TEST_F(PointerDispatcherTest, ScrollLocksItsReceiverForTheGesture) {
  Scroll(&w1_, 150, 150, SignalPhase::kStart);
  Scroll(&w1_, 20, 20, SignalPhase::kUpdate);
  Scroll(&w1_, 20, 20, SignalPhase::kEnd);
  EXPECT_EQ(w1_.TakeNative(), "7:scroll 7:scroll 7:scroll");
  EXPECT_EQ(w1_.TakeMomo(), "");
  Scroll(&w1_, 20, 20, SignalPhase::kNone);
  Scroll(&w1_, 150, 150, SignalPhase::kNone);
  EXPECT_EQ(w1_.TakeMomo(), "add scroll");
  EXPECT_EQ(w1_.TakeNative(), "7:scroll");
}

// 几何变化而指针不动: 没有序列时重新判定悬停;被别的窗口遮住时不算在这个窗口上。
TEST_F(PointerDispatcherTest, GeometryChangesReevaluateTheHover) {
  Move(&w1_, 30, 30);
  w1_.TakeMomo();
  w1_.views[8] = Rect{0, 0, 50, 50};  // 平台视图出现在指针下
  d_.GeometryChanged(&w1_);
  EXPECT_EQ(w1_.TakeMomo(), "remove");
  EXPECT_EQ(w1_.TakeNative(), "hovered 8");
  w1_.views.erase(8);
  d_.GeometryChanged(&w1_);
  EXPECT_EQ(w1_.TakeNative(), "hovered 0");
  EXPECT_EQ(w1_.TakeMomo(), "add hover");
  // 序列期间不重判。
  Down(&w1_, 30, 30);
  w1_.TakeMomo();
  w1_.views[8] = Rect{0, 0, 50, 50};
  d_.GeometryChanged(&w1_);
  EXPECT_EQ(w1_.TakeNative(), "");
  EXPECT_EQ(w1_.TakeMomo(), "");
}

// 移动循环: 每个拖动先移动窗口;系统拖动返回时补报抬起。
TEST_F(PointerDispatcherTest, MoveLoopAndSystemDragEnd) {
  Move(&w1_, 10, 10);
  Down(&w1_, 10, 10);
  EXPECT_FALSE(d_.StartMoveLoop(&w2_, {5, 5}));
  ASSERT_TRUE(d_.StartMoveLoop(&w1_, {10, 10}));
  EXPECT_EQ(w1_.frame.x, 0);
  Move(nullptr, 210, 110, true);
  EXPECT_EQ(w1_.frame.x, 200);
  EXPECT_EQ(w1_.frame.y, 100);
  w1_.TakeMomo();
  d_.EndSequence(&w1_, {210, 110}, ++time_);
  EXPECT_EQ(w1_.TakeMomo(), "up");
  EXPECT_FALSE(d_.has_sequence());
}

}  // namespace
}  // namespace momo::embedder
