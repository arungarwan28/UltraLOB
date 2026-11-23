#include <gtest/gtest.h>

#include <memory>

#include "ultralob/matching_engine.hpp"

namespace {
using namespace ultralob;

// Prices are integer ticks: 10000 == $100.00
constexpr Price k100 = 10000;
constexpr Price k9950 = 9950;
constexpr Price k9900 = 9900;

OrderId newest_maker(const MatchingEngine& engine) {
  Trade trades[8];
  const std::size_t count = engine.recent_trades(trades, 8);
  return count == 0 ? kInvalidId : trades[0].maker;
}
}  // namespace

// The worked example from the specification: a 15-lot market seller against
// bids of 10@100.00, 20@99.50, 15@99.00 must produce 10@100.00 and 5@99.50 and
// leave 15 at 99.50.
TEST(MatchingEngine, MarketSellWalksTheBidStackInPriceOrder) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Buy, k100, 10, 1);
  engine->submit_limit(Side::Buy, k9950, 20, 2);
  engine->submit_limit(Side::Buy, k9900, 15, 3);

  const SubmitResult result = engine->submit_market(Side::Sell, 15, 4);

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.filled, 15u);
  EXPECT_EQ(result.remaining, 0u);
  EXPECT_EQ(result.trade_count, 2u);
  EXPECT_FALSE(result.rested);

  Trade trades[4];
  ASSERT_EQ(engine->recent_trades(trades, 4), 2u);
  EXPECT_EQ(trades[0].price, k9950);  // newest first
  EXPECT_EQ(trades[0].qty, 5u);
  EXPECT_EQ(trades[1].price, k100);
  EXPECT_EQ(trades[1].qty, 10u);

  ASSERT_EQ(engine->depth(Side::Buy), 2u);
  EXPECT_EQ(engine->level(Side::Buy, 0)->price, k9950);
  EXPECT_EQ(engine->level(Side::Buy, 0)->total_qty, 15u);
  EXPECT_EQ(engine->level(Side::Buy, 1)->price, k9900);
  EXPECT_EQ(engine->level(Side::Buy, 1)->total_qty, 15u);
  EXPECT_EQ(engine->active_orders(), 2u);
}

TEST(MatchingEngine, NonCrossingLimitOrderRests) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Sell, 10100, 30, 1);

  const SubmitResult bid = engine->submit_limit(Side::Buy, 10000, 12, 2);

  EXPECT_TRUE(bid.accepted);
  EXPECT_TRUE(bid.rested);
  EXPECT_EQ(bid.filled, 0u);
  EXPECT_EQ(bid.trade_count, 0u);
  EXPECT_EQ(engine->best_bid(), k100);
  EXPECT_EQ(engine->best_ask(), 10100);
  EXPECT_EQ(engine->spread(), 100);
  EXPECT_EQ(engine->active_orders(), 2u);
}

TEST(MatchingEngine, CrossingLimitExecutesThenRestsTheRemainder) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Sell, 10000, 40, 1);

  const SubmitResult buy = engine->submit_limit(Side::Buy, 10100, 100, 2);

  EXPECT_TRUE(buy.accepted);
  EXPECT_EQ(buy.filled, 40u);
  EXPECT_EQ(buy.remaining, 60u);
  EXPECT_TRUE(buy.rested);
  EXPECT_EQ(engine->best_bid(), 10100);  // remainder rests at the limit price
  EXPECT_EQ(engine->depth(Side::Sell), 0u);
}

TEST(MatchingEngine, PriceTimePriorityFillsTheOldestOrderFirst) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult first = engine->submit_limit(Side::Sell, 10000, 10, 1);
  const SubmitResult second = engine->submit_limit(Side::Sell, 10000, 10, 2);
  const SubmitResult third = engine->submit_limit(Side::Sell, 10000, 10, 3);

  const SubmitResult buy = engine->submit_market(Side::Buy, 10, 4);
  EXPECT_EQ(buy.trade_count, 1u);
  EXPECT_EQ(newest_maker(*engine), first.id);

  engine->submit_market(Side::Buy, 10, 5);
  EXPECT_EQ(newest_maker(*engine), second.id);

  Trade trades[8];
  const std::size_t count = engine->recent_trades(trades, 8);
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(trades[1].maker, first.id);
  EXPECT_EQ(trades[0].maker, second.id);
  EXPECT_EQ(engine->find(third.id) != nullptr, true);
}

TEST(MatchingEngine, PartialFillUpdatesRemainingAndLevelTotal) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult maker = engine->submit_limit(Side::Sell, 10000, 50, 1);

  const SubmitResult taker = engine->submit_market(Side::Buy, 20, 2);

  EXPECT_EQ(taker.filled, 20u);
  const Order* resting = engine->find(maker.id);
  ASSERT_NE(resting, nullptr);
  EXPECT_EQ(resting->remaining, 30u);
  EXPECT_EQ(engine->level(Side::Sell, 0)->total_qty, 30u);
  EXPECT_EQ(engine->level(Side::Sell, 0)->order_count, 1u);
}

TEST(MatchingEngine, CancelRemovesLiquidityAndRejectsUnknownIds) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult order = engine->submit_limit(Side::Buy, k100, 25, 1);
  ASSERT_EQ(engine->depth(Side::Buy), 1u);

  const SubmitResult cancelled = engine->cancel(order.id, 2);
  EXPECT_TRUE(cancelled.accepted);
  EXPECT_EQ(cancelled.remaining, 25u);
  EXPECT_EQ(engine->depth(Side::Buy), 0u);
  EXPECT_EQ(engine->active_orders(), 0u);

  const SubmitResult unknown = engine->cancel(424242, 3);
  EXPECT_FALSE(unknown.accepted);
  EXPECT_EQ(unknown.reason, RejectReason::UnknownOrder);
}

TEST(MatchingEngine, ModifyQuantityDownKeepsQueuePriority) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult first = engine->submit_limit(Side::Buy, k100, 10, 1);
  engine->submit_limit(Side::Buy, k100, 10, 2);

  const SubmitResult modified = engine->modify(first.id, k100, 4, 3);
  EXPECT_TRUE(modified.accepted);
  EXPECT_EQ(modified.remaining, 4u);

  engine->submit_market(Side::Sell, 4, 4);
  EXPECT_EQ(newest_maker(*engine), first.id);  // still first in line
  EXPECT_EQ(engine->level(Side::Buy, 0)->total_qty, 10u);  // 14 resting, 4 traded
}

TEST(MatchingEngine, ModifyQuantityUpLosesQueuePriority) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult first = engine->submit_limit(Side::Buy, k100, 10, 1);
  const SubmitResult second = engine->submit_limit(Side::Buy, k100, 10, 2);

  const SubmitResult modified = engine->modify(first.id, k100, 15, 3);
  EXPECT_TRUE(modified.accepted);

  engine->submit_market(Side::Sell, 5, 4);
  EXPECT_EQ(newest_maker(*engine), second.id);  // re-entered behind `second`
}

TEST(MatchingEngine, ModifyPriceChangeReentersAtTail) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult first = engine->submit_limit(Side::Buy, k100, 10, 1);
  const SubmitResult second = engine->submit_limit(Side::Buy, k100, 10, 2);

  const SubmitResult modified = engine->modify(first.id, k9950, 10, 3);
  EXPECT_TRUE(modified.accepted);
  EXPECT_EQ(engine->best_bid(), k100);

  engine->submit_market(Side::Sell, 5, 4);
  EXPECT_EQ(newest_maker(*engine), second.id);

  // The re-priced order is still resting at its new level, quantised exactly.
  const Order* reprice = engine->find(first.id);
  ASSERT_NE(reprice, nullptr);
  EXPECT_EQ(reprice->price, k9950);
}

TEST(MatchingEngine, ModifyCanCrossTheBookAndExecute) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult bid = engine->submit_limit(Side::Buy, k9900, 10, 1);
  const SubmitResult ask = engine->submit_limit(Side::Sell, 10100, 10, 2);

  const SubmitResult modified = engine->modify(ask.id, 9800, 10, 3);

  EXPECT_TRUE(modified.accepted);
  EXPECT_EQ(modified.filled, 10u);  // executed against the resting bid
  Trade trades[4];
  ASSERT_EQ(engine->recent_trades(trades, 4), 1u);
  EXPECT_EQ(trades[0].price, k9900);  // price is the maker's price
  EXPECT_EQ(trades[0].maker, bid.id);
  EXPECT_EQ(engine->active_orders(), 0u);
}

TEST(MatchingEngine, MarketOrderIsImmediateOrCancel) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Sell, 10000, 6, 1);

  const SubmitResult result = engine->submit_market(Side::Buy, 10, 2);

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.filled, 6u);
  EXPECT_EQ(result.remaining, 4u);
  EXPECT_FALSE(result.rested);
  EXPECT_EQ(engine->depth(Side::Buy), 0u);  // nothing rests
  EXPECT_EQ(engine->depth(Side::Sell), 0u);
  EXPECT_EQ(engine->active_orders(), 0u);
}

TEST(MatchingEngine, InvalidOrdersAreRejectedWithoutSideEffects) {
  auto engine = std::make_unique<MatchingEngine>();

  const std::uint64_t before = engine->crc64();

  EXPECT_EQ(engine->submit_limit(Side::Buy, k100, 0, 1).reason,
            RejectReason::InvalidQuantity);
  EXPECT_EQ(engine->submit_limit(Side::Buy, 0, 10, 2).reason, RejectReason::InvalidPrice);
  EXPECT_EQ(engine->submit_limit(Side::Buy, -1, 10, 3).reason, RejectReason::InvalidPrice);
  EXPECT_EQ(engine->submit_market(Side::Buy, 0, 4).reason, RejectReason::InvalidQuantity);

  EXPECT_EQ(engine->crc64(), before) << "rejects must not mutate the book";

  engine->submit_limit(Side::Buy, k100, 10, 5, /*id=*/77);
  const std::uint64_t after_valid = engine->crc64();
  EXPECT_EQ(engine->submit_limit(Side::Sell, 10100, 10, 6, /*id=*/77).reason,
            RejectReason::DuplicateOrderId);
  EXPECT_EQ(engine->crc64(), after_valid);

  EXPECT_EQ(engine->stats().rejects, 5u);
  EXPECT_EQ(engine->active_orders(), 1u);
}

TEST(MatchingEngine, EmptyBookMarketOrderIsCancelledNotResting) {
  auto engine = std::make_unique<MatchingEngine>();
  const SubmitResult result = engine->submit_market(Side::Buy, 10, 1);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.filled, 0u);
  EXPECT_EQ(result.remaining, 10u);
  EXPECT_EQ(engine->active_orders(), 0u);
}

TEST(MatchingEngine, SpreadIsUndefinedWithOneSidedBook) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Buy, k100, 10, 1);
  EXPECT_EQ(engine->spread(), -1);
  EXPECT_TRUE(engine->has_bid());
  EXPECT_FALSE(engine->has_ask());
}

TEST(MatchingEngine, MultipleLevelsTradeAcrossTheFullDepth) {
  auto engine = std::make_unique<MatchingEngine>();
  for (Price offset = 0; offset < 5; ++offset) {
    engine->submit_limit(Side::Sell, 10000 + offset * 10, 10, static_cast<Seq>(offset + 1));
  }

  const SubmitResult sweep = engine->submit_market(Side::Buy, 45, 100);
  EXPECT_EQ(sweep.filled, 45u);
  EXPECT_EQ(sweep.trade_count, 5u);
  EXPECT_EQ(engine->depth(Side::Sell), 1u);
  EXPECT_EQ(engine->level(Side::Sell, 0)->total_qty, 5u);
  EXPECT_EQ(engine->level(Side::Sell, 0)->price, 10040);
}
