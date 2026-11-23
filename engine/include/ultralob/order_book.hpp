// UltraLOB — order book (resting state only).
//
// The book owns price levels and the FIFO linkage of resting orders. It does
// not match anything: matching lives in MatchingEngine, so the book can be
// tested and snapshotted on its own.
#pragma once

#include <cstddef>

#include "ultralob/config.hpp"
#include "ultralob/level_table.hpp"
#include "ultralob/memory_pool.hpp"
#include "ultralob/order.hpp"
#include "ultralob/order_index.hpp"
#include "ultralob/price_level.hpp"
#include "ultralob/types.hpp"

namespace ultralob {

class OrderBook {
 public:
  using Pool = ObjectPool<Order, Limits::kMaxOrders>;

  explicit OrderBook(Pool& pool) noexcept : pool_(&pool) {}

  void reset() noexcept;

  // Adds a resting order at the FIFO tail of its level. False when the level
  // table for that side is full (the caller rolls the pool slot back).
  bool add(Order& order) noexcept;

  // Unlinks a resting order and releases its pool slot. Also erases the level
  // when it becomes empty. `order` may be a stale copy: only its slot is used
  // (the authoritative record is read back from the pool).
  // Requires pool.add() to have been called on the same slot before this; the
  // order cannot have been removed yet.
  void remove(Order& order) noexcept;

  bool empty(Side side) const noexcept {
    return table(side).size() == 0;
  }
  const PriceLevel* best(Side side) const noexcept;
  PriceLevel* best_mut(Side side) noexcept;
  const PriceLevel* level(Side side, std::size_t index) const noexcept;
  std::size_t depth(Side side) const noexcept { return table(side).size(); }

  // Aggregate quantity resting at an exact price (0 when absent).
  Qty qty_at(Side side, Price price) const noexcept;

  // Level lookup by price: O(log levels). Removing an order therefore never has
  // to trust a cached level index that array compaction could invalidate.
  PriceLevel* find_level(Side side, Price price) noexcept;
  const PriceLevel* find_level(Side side, Price price) const noexcept;

  std::size_t resting_orders() const noexcept { return index_.size(); }

  Pool& pool() noexcept { return *pool_; }
  const Pool& pool() const noexcept { return *pool_; }
  const OrderIndex& index() const noexcept { return index_; }

  const Order* find(OrderId id) const noexcept;
  Order* find(OrderId id) noexcept;

 private:
  LevelTable& table(Side side) noexcept {
    return side == Side::Buy ? bids_ : asks_;
  }
  const LevelTable& table(Side side) const noexcept {
    return side == Side::Buy ? bids_ : asks_;
  }

  Pool* pool_;
  OrderIndex index_;
  LevelTable bids_{Side::Buy};
  LevelTable asks_{Side::Sell};
};

}  // namespace ultralob
