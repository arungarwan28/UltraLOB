#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "ultralob/feed_generator.hpp"
#include "ultralob/matching_engine.hpp"

namespace {
using namespace ultralob;

// Independent structural check of the book: level ordering, aggregate totals and
// FIFO linkage must agree with what the engine reports.
void check_book_invariants(const MatchingEngine& engine) {
  for (const Side side : {Side::Buy, Side::Sell}) {
    Price previous = 0;
    Qty running_total = 0;
    std::size_t orders_seen = 0;
    for (std::size_t level_index = 0; level_index < engine.depth(side); ++level_index) {
      const PriceLevel* level = engine.level(side, level_index);
      ASSERT_NE(level, nullptr);
      if (level_index > 0) {
        if (side == Side::Buy) {
          EXPECT_LT(level->price, previous) << "bid levels must descend";
        } else {
          EXPECT_GT(level->price, previous) << "ask levels must ascend";
        }
      }
      previous = level->price;

      Qty level_sum = 0;
      const std::size_t count = engine.level_order_count(side, level_index);
      EXPECT_EQ(count, level->order_count);
      for (std::size_t k = 0; k < count; ++k) {
        const Order* order = engine.level_order_at(side, level_index, k);
        ASSERT_NE(order, nullptr);
        EXPECT_EQ(order->price, level->price);
        EXPECT_EQ(order->side, side);
        EXPECT_GT(order->remaining, 0u);
        level_sum += order->remaining;
        ++orders_seen;
      }
      EXPECT_EQ(level_sum, level->total_qty);
      running_total += level_sum;
    }
    (void)orders_seen;
  }

  if (engine.active_orders() > 0) {
    std::size_t total_orders = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (std::size_t i = 0; i < engine.depth(side); ++i) {
        total_orders += engine.level_order_count(side, i);
      }
    }
    EXPECT_EQ(total_orders, engine.active_orders());
  }
}
}  // namespace

TEST(OrderBook, LevelsAreSortedBestPriceFirst) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Buy, 9900, 5, 1);
  engine->submit_limit(Side::Buy, 10000, 5, 2);
  engine->submit_limit(Side::Buy, 9950, 5, 3);
  engine->submit_limit(Side::Sell, 10100, 5, 4);
  engine->submit_limit(Side::Sell, 10050, 5, 5);
  engine->submit_limit(Side::Sell, 10200, 5, 6);

  ASSERT_EQ(engine->depth(Side::Buy), 3u);
  EXPECT_EQ(engine->level(Side::Buy, 0)->price, 10000);
  EXPECT_EQ(engine->level(Side::Buy, 1)->price, 9950);
  EXPECT_EQ(engine->level(Side::Buy, 2)->price, 9900);

  ASSERT_EQ(engine->depth(Side::Sell), 3u);
  EXPECT_EQ(engine->level(Side::Sell, 0)->price, 10050);
  EXPECT_EQ(engine->level(Side::Sell, 1)->price, 10100);
  EXPECT_EQ(engine->level(Side::Sell, 2)->price, 10200);
  check_book_invariants(*engine);
}

TEST(OrderBook, EmptyLevelIsErased) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Sell, 10000, 10, 1);
  engine->submit_limit(Side::Sell, 10010, 10, 2);
  ASSERT_EQ(engine->depth(Side::Sell), 2u);

  engine->submit_market(Side::Buy, 10, 3);
  ASSERT_EQ(engine->depth(Side::Sell), 1u);
  EXPECT_EQ(engine->level(Side::Sell, 0)->price, 10010);
  check_book_invariants(*engine);
}

TEST(OrderBook, AggregateQuantityAtPrice) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Buy, 10000, 10, 1);
  engine->submit_limit(Side::Buy, 10000, 15, 2);
  engine->submit_limit(Side::Buy, 10000, 5, 3);

  EXPECT_EQ(engine->book().qty_at(Side::Buy, 10000), 30u);
  EXPECT_EQ(engine->book().qty_at(Side::Buy, 9999), 0u);
  EXPECT_EQ(engine->level(Side::Buy, 0)->order_count, 3u);
}

TEST(OrderBook, FindReturnsRestingOrderAndNullAfterCancel) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult order = engine->submit_limit(Side::Buy, 10000, 10, 1);

  const Order* resting = engine->find(order.id);
  ASSERT_NE(resting, nullptr);
  EXPECT_EQ(resting->id, order.id);
  EXPECT_EQ(resting->remaining, 10u);

  engine->cancel(order.id, 2);
  EXPECT_EQ(engine->find(order.id), nullptr);
}

TEST(OrderBook, InvariantsHoldUnderGeneratedFlow) {
  auto engine = std::make_unique<MatchingEngine>();
  FeedGenerator generator(workload_config(Workload::Balanced, 0x1234));

  for (int i = 0; i < 4000; ++i) {
    const Event event = generator.next();
    switch (event.type) {
      case EventType::NewOrder:
        if (event.order_type == OrderType::Market) {
          engine->submit_market(event.side, event.qty, event.seq, event.order_id);
        } else {
          engine->submit_limit(event.side, event.price, event.qty, event.seq,
                               event.order_id);
        }
        break;
      case EventType::CancelOrder:
        engine->cancel(event.order_id, event.seq);
        break;
      case EventType::ModifyOrder:
        engine->modify(event.order_id, event.price, event.qty, event.seq);
        break;
      default:
        break;
    }
    if (i % 250 == 0) check_book_invariants(*engine);
  }
  check_book_invariants(*engine);
  EXPECT_GT(engine->trade_count(), 0u);
  EXPECT_GT(engine->active_orders(), 0u);
}

TEST(OrderBook, IndexSurvivesHeavyChurn) {
  auto engine = std::make_unique<MatchingEngine>();
  std::vector<OrderId> live;
  live.reserve(2000);

  // Add and cancel in a pattern that repeatedly frees and reuses pool slots and
  // hash index entries (backward-shift deletion path).
  for (int round = 0; round < 20; ++round) {
    for (int i = 0; i < 500; ++i) {
      const SubmitResult result =
          engine->submit_limit(Side::Buy, 9000 + (i % 20), 1, static_cast<Seq>(i + 1));
      ASSERT_TRUE(result.accepted);
      live.push_back(result.id);
    }
    for (std::size_t i = 0; i < live.size(); i += 2) {
      EXPECT_TRUE(engine->cancel(live[i], 1).accepted);
    }
    std::vector<OrderId> survivors;
    for (std::size_t i = 1; i < live.size(); i += 2) survivors.push_back(live[i]);
    live.swap(survivors);
  }

  EXPECT_EQ(engine->active_orders(), live.size());
  for (const OrderId id : live) EXPECT_NE(engine->find(id), nullptr);
  check_book_invariants(*engine);
}
