#include "base/pipeline.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace momo {
namespace {

using IntPipeline = Pipeline<std::unique_ptr<int>>;

TEST(PipelineTest, ProduceFailsWhenFull) {
  auto pipeline = IntPipeline::Create(2);
  auto a = pipeline->Produce();
  auto b = pipeline->Produce();
  EXPECT_TRUE(a);
  EXPECT_TRUE(b);
  EXPECT_FALSE(pipeline->Produce());
  EXPECT_EQ(pipeline->available_slots(), 0u);
}

TEST(PipelineTest, DroppedContinuationReleasesSlot) {
  auto pipeline = IntPipeline::Create(1);
  { auto c = pipeline->Produce(); EXPECT_TRUE(c); }
  EXPECT_EQ(pipeline->available_slots(), 1u);
  EXPECT_TRUE(pipeline->Produce());
}

TEST(PipelineTest, SlotIsHeldUntilConsumerFinishes) {
  auto pipeline = IntPipeline::Create(1);
  EXPECT_TRUE(pipeline->Produce().Complete(std::make_unique<int>(7)));
  EXPECT_EQ(pipeline->queued(), 1u);
  EXPECT_FALSE(pipeline->Produce());  // 已入队, 槽位未归还

  bool inside_checked = false;
  const auto result = pipeline->Consume([&](std::unique_ptr<int> v) {
    EXPECT_EQ(*v, 7);
    EXPECT_EQ(pipeline->available_slots(), 0u);  // 消费中仍占用
    inside_checked = true;
  });
  EXPECT_TRUE(inside_checked);
  EXPECT_EQ(result, PipelineConsumeResult::kDone);
  EXPECT_EQ(pipeline->available_slots(), 1u);
}

TEST(PipelineTest, ConsumeIsFifoAndReportsMore) {
  auto pipeline = IntPipeline::Create(3);
  for (int i = 0; i < 3; ++i) {
    pipeline->Produce().Complete(std::make_unique<int>(i));
  }
  std::vector<int> seen;
  auto take = [&](std::unique_ptr<int> v) { seen.push_back(*v); };
  EXPECT_EQ(pipeline->Consume(take), PipelineConsumeResult::kMoreAvailable);
  EXPECT_EQ(pipeline->Consume(take), PipelineConsumeResult::kMoreAvailable);
  EXPECT_EQ(pipeline->Consume(take), PipelineConsumeResult::kDone);
  EXPECT_EQ(pipeline->Consume(take), PipelineConsumeResult::kNoItem);
  EXPECT_EQ(seen, (std::vector<int>{0, 1, 2}));
}

TEST(PipelineTest, CompleteOnEmptyContinuationFails) {
  IntPipeline::ProducerContinuation empty;
  EXPECT_FALSE(empty.Complete(std::make_unique<int>(1)));
}

TEST(PipelineTest, ContinuationKeepsPipelineAlive) {
  std::weak_ptr<IntPipeline> weak;
  IntPipeline::ProducerContinuation c;
  {
    auto pipeline = IntPipeline::Create(1);
    weak = pipeline;
    c = pipeline->Produce();
  }
  EXPECT_FALSE(weak.expired());
  c = IntPipeline::ProducerContinuation();
  EXPECT_TRUE(weak.expired());
}

TEST(PipelineTest, ConcurrentProducerAndConsumerNeverExceedDepth) {
  constexpr int kItems = 5000;
  auto pipeline = IntPipeline::Create(2);
  std::atomic<int> consumed{0};
  std::atomic<bool> done{false};

  std::thread consumer([&] {
    while (!done.load() || pipeline->queued() > 0) {
      pipeline->Consume([&](std::unique_ptr<int> v) {
        EXPECT_EQ(*v, consumed.load());
        consumed++;
      });
    }
  });

  int produced = 0;
  while (produced < kItems) {
    if (auto c = pipeline->Produce()) {
      EXPECT_LE(pipeline->queued(), 2u);
      c.Complete(std::make_unique<int>(produced++));
    } else {
      std::this_thread::yield();
    }
  }
  done = true;
  consumer.join();
  EXPECT_EQ(consumed.load(), kItems);
  EXPECT_EQ(pipeline->available_slots(), 2u);
}

}  // namespace
}  // namespace momo
