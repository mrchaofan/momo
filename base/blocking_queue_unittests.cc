#include "base/blocking_queue.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace momo {
namespace {

TEST(BlockingQueueTest, FifoAndTryPop) {
  BlockingQueue<int> queue;
  EXPECT_FALSE(queue.TryPop().has_value());
  EXPECT_TRUE(queue.Push(1));
  EXPECT_TRUE(queue.Push(2));
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.TryPop(), 1);
  EXPECT_EQ(queue.Pop(), 2);
  EXPECT_EQ(queue.size(), 0u);
}

TEST(BlockingQueueTest, MoveOnlyValues) {
  BlockingQueue<std::unique_ptr<std::string>> queue;
  queue.Push(std::make_unique<std::string>("hello"));
  std::optional<std::unique_ptr<std::string>> value = queue.Pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(**value, "hello");
}

TEST(BlockingQueueTest, PopForTimesOut) {
  BlockingQueue<int> queue;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(queue.PopFor(std::chrono::milliseconds(20)).has_value());
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(15));
}

TEST(BlockingQueueTest, PopBlocksUntilProducerPushes) {
  BlockingQueue<int> queue;
  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    queue.Push(42);
  });
  EXPECT_EQ(queue.Pop(), 42);
  producer.join();
}

TEST(BlockingQueueTest, CloseWakesConsumersAndKeepsQueuedItems) {
  BlockingQueue<int> queue;
  queue.Push(7);
  queue.Close();
  queue.Close();  // 幂等
  EXPECT_TRUE(queue.closed());
  EXPECT_FALSE(queue.Push(8));
  EXPECT_EQ(queue.Pop(), 7);               // 关闭前入队的仍可取出
  EXPECT_FALSE(queue.Pop().has_value());   // 取空后不再阻塞

  BlockingQueue<int> empty;
  std::thread closer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    empty.Close();
  });
  EXPECT_FALSE(empty.Pop().has_value());  // 被 Close 唤醒
  closer.join();
}

TEST(BlockingQueueTest, ManyProducersManyConsumers) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 3;
  constexpr int kPerProducer = 2000;
  BlockingQueue<int> queue;
  std::vector<std::thread> threads;
  std::vector<long long> sums(kConsumers, 0);
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&, c] {
      while (std::optional<int> v = queue.Pop()) {
        sums[c] += *v;
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&] {
      for (int i = 1; i <= kPerProducer; ++i) {
        queue.Push(i);
      }
    });
  }
  for (auto& t : producers) {
    t.join();
  }
  queue.Close();
  for (auto& t : threads) {
    t.join();
  }
  long long total = 0;
  for (long long s : sums) {
    total += s;
  }
  EXPECT_EQ(total, static_cast<long long>(kProducers) * kPerProducer * (kPerProducer + 1) / 2);
}

}  // namespace
}  // namespace momo
