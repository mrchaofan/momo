// 跨视图指针捕获: 按下所在视图独占该设备的事件直到抬起。

#include "engine/shell/pointer_capture.h"

#include "gtest/gtest.h"

namespace momo::engine {
namespace {

using platform::PointerData;
using platform::PointerDataPacket;
using platform::PointerPhase;

PointerData Event(PointerPhase phase, int32_t device = 0) {
  PointerData p;
  p.phase = phase;
  p.device = device;
  return p;
}

TEST(PointerCaptureTest, OtherViewsAreIgnoredWhileAButtonIsHeld) {
  PointerCapture capture;
  EXPECT_EQ(capture.Filter(0, {Event(PointerPhase::kAdd), Event(PointerPhase::kDown)}).size(), 2u);
  EXPECT_EQ(capture.captured_by(0), 0);
  // 拖动经过视图 1: 它上报的 add / hover / remove 都被丢弃(否则框架会取消进行中的拖动)。
  EXPECT_TRUE(capture.Filter(1, {Event(PointerPhase::kAdd), Event(PointerPhase::kHover)}).empty());
  EXPECT_TRUE(capture.Filter(1, {Event(PointerPhase::kRemove)}).empty());
  EXPECT_TRUE(capture.Filter(1, {Event(PointerPhase::kScroll)}).empty());
  // 捕获视图的移动与抬起照常交付;抬起后释放。
  EXPECT_EQ(capture.Filter(0, {Event(PointerPhase::kMove)}).size(), 1u);
  EXPECT_EQ(capture.Filter(0, {Event(PointerPhase::kUp)}).size(), 1u);
  EXPECT_FALSE(capture.captured_by(0).has_value());
  EXPECT_EQ(capture.Filter(1, {Event(PointerPhase::kHover)}).size(), 1u);
}

TEST(PointerCaptureTest, CaptureIsPerDevice) {
  PointerCapture capture;
  capture.Filter(0, {Event(PointerPhase::kDown, 0)});
  EXPECT_EQ(capture.Filter(1, {Event(PointerPhase::kDown, 1)}).size(), 1u);
  EXPECT_EQ(capture.captured_by(1), 1);
  EXPECT_TRUE(capture.Filter(1, {Event(PointerPhase::kMove, 0)}).empty());
}

TEST(PointerCaptureTest, RemovingTheCapturingViewReleasesAndSynthesizesRemove) {
  PointerCapture capture;
  capture.Filter(2, {Event(PointerPhase::kDown)});
  EXPECT_TRUE(capture.ReleaseView(1).empty());
  const PointerDataPacket synthesized = capture.ReleaseView(2);
  ASSERT_EQ(synthesized.size(), 1u);
  EXPECT_EQ(synthesized[0].phase, PointerPhase::kRemove);
  EXPECT_FALSE(capture.captured_by(0).has_value());
  EXPECT_EQ(capture.Filter(0, {Event(PointerPhase::kHover)}).size(), 1u);
}

// view.setCapture(): 进行中的序列转交给另一个视图;原视图之后的事件作废, 移除原视图也不再补发 remove。
TEST(PointerCaptureTest, TransferHandsTheSequenceToAnotherView) {
  PointerCapture capture;
  EXPECT_FALSE(capture.TransferTo(1)) << "nothing pressed: nothing to transfer";
  capture.Filter(0, {Event(PointerPhase::kDown)});
  EXPECT_TRUE(capture.TransferTo(1));
  EXPECT_EQ(capture.captured_by(0), 1);
  EXPECT_TRUE(capture.Filter(0, {Event(PointerPhase::kMove), Event(PointerPhase::kUp)}).empty());
  EXPECT_TRUE(capture.ReleaseView(0).empty());
  // 新视图补发的按下与之后的拖动、抬起照常交付。
  EXPECT_EQ(capture.Filter(1, {Event(PointerPhase::kDown), Event(PointerPhase::kMove)}).size(), 2u);
  EXPECT_EQ(capture.Filter(1, {Event(PointerPhase::kUp)}).size(), 1u);
  EXPECT_FALSE(capture.captured_by(0).has_value());
}

}  // namespace
}  // namespace momo::engine
