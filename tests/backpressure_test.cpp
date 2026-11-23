#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/spsc_queue.hpp"

namespace {
using namespace ultralob;

EventProcessor::Config processor_config() {
  EventProcessor::Config config{};
  config.gap_policy = GapPolicy::Buffer;
  config.duplicate_policy = DuplicatePolicy::Reject;
  config.start_seq = 1;
  return config;
}

std::uint64_t baseline_crc(std::uint64_t seed, std::size_t count) {
  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, processor_config());
  FeedGenerator generator(workload_config(Workload::Balanced, seed));
  for (std::size_t i = 0; i < count; ++i) processor.process(generator.next());
  return engine->crc64();
}
}  // namespace

TEST(SpscQueue, PreservesFifoOrderAndReportsFull) {
  SpscQueue<int, 8> queue;  // 7 usable slots
  EXPECT_EQ(queue.capacity(), 7u);

  for (int i = 0; i < 7; ++i) EXPECT_TRUE(queue.try_push(i));
  EXPECT_FALSE(queue.try_push(7));
  EXPECT_EQ(queue.full_events(), 1u);
  EXPECT_EQ(queue.size(), 7u);

  int value = -1;
  for (int i = 0; i < 7; ++i) {
    ASSERT_TRUE(queue.try_pop(value));
    EXPECT_EQ(value, i);
  }
  EXPECT_FALSE(queue.try_pop(value));
  EXPECT_TRUE(queue.empty());
}

TEST(Backpressure, RetryPolicyDeliversEveryEventWithoutLoss) {
  constexpr std::size_t kEvents = 100000;
  constexpr std::size_t kSeed = 0xC0DE;

  SpscQueue<Event, 64> queue;
  BoundedProducer<Event, 64> producer(queue, ProducerPolicy::Retry);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, processor_config());
  std::atomic<std::uint64_t> applied{0};
  std::atomic<bool> consumer_done{false};

  std::thread consumer([&] {
    Event event{};
    std::uint64_t popped_since_pause = 0;
    while (applied.load(std::memory_order_relaxed) < kEvents) {
      if (queue.try_pop(event)) {
        processor.process(event);
        applied.fetch_add(1, std::memory_order_relaxed);
        if (++popped_since_pause % 8192 == 0) {
          // Deliberately slow consumer: forces the producer into retries.
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      } else {
        std::this_thread::yield();
      }
    }
    consumer_done.store(true);
  });

  FeedGenerator generator(workload_config(Workload::Balanced, kSeed));
  for (std::size_t i = 0; i < kEvents; ++i) {
    const bool pushed = producer.push(generator.next());
    ASSERT_TRUE(pushed) << "retry policy must never drop";
  }

  consumer.join();

  EXPECT_TRUE(consumer_done.load());
  EXPECT_EQ(applied.load(), kEvents);
  EXPECT_EQ(producer.counters().pushed, kEvents);
  EXPECT_EQ(producer.counters().dropped, 0u);
  EXPECT_GT(producer.counters().retries, 0u) << "queue pressure was expected";
  EXPECT_GT(queue.full_events(), 0u) << "queue pressure was expected";
  EXPECT_EQ(processor.stats().applied, kEvents);

  // Order preserved through the queue: identical to applying the feed directly.
  EXPECT_EQ(engine->crc64(), baseline_crc(kSeed, kEvents));
}

TEST(Backpressure, DropPolicyAccountsForEveryDiscardedEvent) {
  constexpr std::size_t kEvents = 20000;
  constexpr std::size_t kSeed = 0xD00D;

  SpscQueue<Event, 32> queue;
  BoundedProducer<Event, 32> producer(queue, ProducerPolicy::Drop);

  FeedGenerator generator(workload_config(Workload::Balanced, kSeed));
  std::uint64_t consumed = 0;
  for (std::size_t i = 0; i < kEvents; ++i) {
    producer.push(generator.next());
    if (i % 64 == 0) {
      Event event{};
      if (queue.try_pop(event)) ++consumed;
    }
  }
  Event event{};
  while (queue.try_pop(event)) ++consumed;

  const auto& counters = producer.counters();
  EXPECT_GT(counters.dropped, 0u);
  EXPECT_EQ(counters.pushed + counters.dropped, kEvents);
  EXPECT_EQ(counters.pushed, consumed);
}
