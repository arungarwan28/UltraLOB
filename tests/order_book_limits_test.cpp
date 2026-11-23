// Capacity and rollback behaviour of the book.
//
// Regression: if the id index rejected an insert *after* the level table had
// created a level, the empty level stayed in the book. best_bid()/best_ask()
// then reported a price with no quantity and matching stopped at a level with
// no FIFO head.
#include <gtest/gtest.h>

#include <memory>

#include "ultralob/config.hpp"
#include "ultralob/order_book.hpp"

namespace {

using namespace ultralob;
using TestPool = ObjectPool<Order, Limits::kMaxOrders>;

// The pool holds kMaxOrders full Order records (~8 MB); it must not live on the
// test thread's stack.
Order make_order(TestPool& pool, OrderId id, Price price, Qty qty, Side side) {
  Order order{};
  order.id = id;
  order.price = price;
  order.qty = qty;
  order.remaining = qty;
  order.seq = 1;
  order.side = side;
  order.slot = pool.acquire();
  return order;
}

}  // namespace

TEST(OrderBookLimits, RejectedDuplicateIdLeavesNoPhantomLevel) {
  auto pool = std::make_unique<TestPool>();
  OrderBook book(*pool);

  Order first = make_order(*pool, 7, 10000, 10, Side::Buy);
  ASSERT_TRUE(book.add(first));
  ASSERT_EQ(book.depth(Side::Buy), 1u);

  // Same id, different price: the level is created, then the index refuses the
  // duplicate id. The level must be rolled back.
  Order duplicate = make_order(*pool, 7, 10050, 5, Side::Buy);
  EXPECT_FALSE(book.add(duplicate));

  EXPECT_EQ(book.depth(Side::Buy), 1u) << "rejected add left an empty level behind";
  const PriceLevel* best = book.best(Side::Buy);
  ASSERT_NE(best, nullptr);
  EXPECT_EQ(best->price, 10000);
  EXPECT_EQ(best->total_qty, 10u);
  EXPECT_EQ(best->order_count, 1u);

  // The caller owns the rejected order's pool slot and releases it.
  pool->release(duplicate.slot);
  EXPECT_EQ(book.resting_orders(), 1u);

  book.remove(first);
  EXPECT_EQ(book.depth(Side::Buy), 0u);
  EXPECT_TRUE(book.empty(Side::Buy));
}

TEST(OrderBookLimits, LevelTableFullRejectsWithoutMutatingTheBook) {
  auto pool = std::make_unique<TestPool>();
  OrderBook book(*pool);

  for (std::size_t i = 0; i < Limits::kMaxLevelsPerSide; ++i) {
    Order order = make_order(*pool, static_cast<OrderId>(i + 1), 1000 + static_cast<Price>(i), 1,
                             Side::Buy);
    ASSERT_TRUE(book.add(order)) << "level " << i;
  }
  ASSERT_EQ(book.depth(Side::Buy), Limits::kMaxLevelsPerSide);

  Order extra = make_order(*pool, 999'999, 500'000, 1, Side::Buy);
  EXPECT_FALSE(book.add(extra));
  EXPECT_EQ(book.depth(Side::Buy), Limits::kMaxLevelsPerSide);
  EXPECT_EQ(book.resting_orders(), Limits::kMaxLevelsPerSide);
  EXPECT_EQ(book.best(Side::Buy)->price, 1000 + static_cast<Price>(Limits::kMaxLevelsPerSide - 1));
  pool->release(extra.slot);
}

TEST(OrderBookLimits, RemoveErasesLevelAndReleasesSlot) {
  auto pool = std::make_unique<TestPool>();
  OrderBook book(*pool);

  Order a = make_order(*pool, 1, 10000, 4, Side::Sell);
  Order b = make_order(*pool, 2, 10000, 6, Side::Sell);
  ASSERT_TRUE(book.add(a));
  ASSERT_TRUE(book.add(b));
  ASSERT_EQ(book.depth(Side::Sell), 1u);
  ASSERT_EQ(book.best(Side::Sell)->total_qty, 10u);
  ASSERT_EQ(book.best(Side::Sell)->order_count, 2u);

  book.remove(a);
  EXPECT_EQ(book.best(Side::Sell)->total_qty, 6u);
  EXPECT_EQ(book.best(Side::Sell)->order_count, 1u);
  EXPECT_EQ(book.best(Side::Sell)->head, b.slot);
  EXPECT_EQ(book.resting_orders(), 1u);

  book.remove(b);
  EXPECT_EQ(book.depth(Side::Sell), 0u);
  EXPECT_EQ(book.resting_orders(), 0u);
  EXPECT_EQ(book.best(Side::Sell), nullptr);
}
