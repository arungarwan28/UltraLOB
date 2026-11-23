#include <gtest/gtest.h>

#include <memory>

#include "alloc_counter.hpp"
#include "ultralob/memory_pool.hpp"
#include "ultralob/replay.hpp"

namespace {
using namespace ultralob;

struct Widget {
  std::uint32_t value = 0;
};
}  // namespace

TEST(ObjectPool, AcquireReleaseAndHighWaterMark) {
  ObjectPool<Widget, 4> pool;

  const std::uint32_t a = pool.acquire();
  const std::uint32_t b = pool.acquire();
  EXPECT_NE(a, kNoIndex);
  EXPECT_NE(b, kNoIndex);
  EXPECT_NE(a, b);
  EXPECT_EQ(pool.live(), 2u);

  pool[a].value = 7;
  EXPECT_EQ(pool[a].value, 7u);
  EXPECT_TRUE(pool.is_live(a));

  pool.release(a);
  EXPECT_FALSE(pool.is_live(a));
  EXPECT_EQ(pool.live(), 1u);
  EXPECT_EQ(pool.high_water_mark(), 2u);

  // Freed slots are reused before the pool grows.
  const std::uint32_t c = pool.acquire();
  EXPECT_EQ(c, a);
  EXPECT_EQ(pool.live(), 2u);

  pool.release(c);
  pool.release(c);  // double release is a no-op, not corruption
  EXPECT_EQ(pool.live(), 1u);
}

TEST(ObjectPool, ExhaustionIsReportedNotThrown) {
  ObjectPool<Widget, 2> pool;
  EXPECT_NE(pool.acquire(), kNoIndex);
  EXPECT_NE(pool.acquire(), kNoIndex);
  EXPECT_EQ(pool.acquire(), kNoIndex);
  EXPECT_EQ(pool.exhausted_events(), 1u);
  EXPECT_EQ(pool.live(), 2u);
}

TEST(Memory, SteadyStateMatchingDoesNotTouchTheHeap) {
  auto engine = std::make_unique<MatchingEngine>();

  ReplayConfig config{};
  config.feed = workload_config(Workload::Balanced, 0x99);
  config.processor.gap_policy = GapPolicy::Buffer;

  ReplaySession session(*engine, config);
  session.run(2000);  // warm up, then measure only the steady state

  ultralob_test::reset_allocation_stats();
  session.run(20000);
  const ultralob_test::AllocStats stats = ultralob_test::allocation_stats();

  EXPECT_EQ(stats.allocations, 0u)
      << "engine, event processor and replay loop allocated "
      << stats.allocations << " times after construction";
  EXPECT_GT(engine->active_orders(), 0u);
  EXPECT_GT(engine->trade_count(), 0u);
}

TEST(Memory, OrderPoolReuseIsBounded) {
  auto engine = std::make_unique<MatchingEngine>();

  ReplayConfig config{};
  config.feed = workload_config(Workload::CancelHeavy, 0x77);
  ReplaySession session(*engine, config);
  session.run(30000);

  const OrderBook::Pool& pool = engine->book().pool();
  EXPECT_EQ(pool.live(), engine->active_orders());
  EXPECT_LT(pool.high_water_mark(), OrderBook::Pool::capacity());
  EXPECT_EQ(pool.exhausted_events(), 0u);
}
