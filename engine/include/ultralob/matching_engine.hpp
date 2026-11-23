// UltraLOB — matching engine.
//
// Price-time priority, integer prices, deterministic output. The engine performs
// no heap allocation after construction: orders come from a fixed pool, levels
// from fixed tables, trades from a fixed ring.
//
// Documented policies (see docs/design-decisions.md):
//  * Market orders never rest. Unfilled remainder is cancelled (IOC).
//  * A limit order that crosses executes first and only the remainder rests.
//  * Modify that keeps price and does not increase quantity keeps queue
//    priority; any other modify loses priority (remove + re-enter at tail,
//    matching as a taker if it crosses).
//  * Self-trade is allowed (no self-trade prevention) — this is a simulator.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ultralob/config.hpp"
#include "ultralob/crc64.hpp"
#include "ultralob/memory_pool.hpp"
#include "ultralob/order.hpp"
#include "ultralob/order_book.hpp"
#include "ultralob/price_level.hpp"
#include "ultralob/types.hpp"

namespace ultralob {

enum class RejectReason : std::uint8_t {
  None = 0,
  InvalidQuantity = 1,
  InvalidPrice = 2,
  DuplicateOrderId = 3,
  UnknownOrder = 4,
  OrderNotResting = 5,
  BookFull = 6,
  OrderPoolExhausted = 7,
  IndexFull = 8,
};

const char* reject_reason_name(RejectReason reason) noexcept;

struct SubmitResult {
  bool accepted = false;
  RejectReason reason = RejectReason::None;
  OrderId id = kInvalidId;
  Qty filled = 0;
  Qty remaining = 0;      // unfilled quantity: resting (limit) or cancelled (market)
  std::uint32_t trade_count = 0;
  bool rested = false;
};

struct Trade {
  TradeId id = 0;
  OrderId maker = kInvalidId;
  OrderId taker = kInvalidId;
  Price price = 0;
  Qty qty = 0;
  Seq seq = 0;
  Side taker_side = Side::Buy;
};

class MatchingEngine {
 public:
  struct Stats {
    std::uint64_t submissions = 0;
    std::uint64_t rejects = 0;
    std::uint64_t cancels = 0;
    std::uint64_t modifies = 0;
    std::uint64_t fills = 0;       // maker orders fully removed
    std::uint64_t executions = 0;  // individual maker/taker executions
    std::uint64_t trades = 0;      // same count, kept for report symmetry
    std::uint64_t traded_qty = 0;
    std::uint64_t traded_notional = 0;  // ticks * qty
  };

  MatchingEngine() noexcept;
  ~MatchingEngine();

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  void reset() noexcept;

  SubmitResult submit_limit(Side side, Price price, Qty qty, Seq seq,
                            OrderId id = kInvalidId) noexcept;
  SubmitResult submit_market(Side side, Qty qty, Seq seq,
                             OrderId id = kInvalidId) noexcept;
  SubmitResult cancel(OrderId id, Seq seq) noexcept;
  SubmitResult modify(OrderId id, Price price, Qty qty, Seq seq) noexcept;

  // Snapshot restore path: restores the trade counter from a book image. The
  // trade tape itself is display data and is not serialized, so recent_trades()
  // restarts from the checkpoint. Documented in docs/reliability.md.
  void restore_trade_sequence(std::uint64_t trades) noexcept {
    trade_sequence_ = trades;
    trades_logged_ = 0;
    trade_head_ = 0;
  }

  // Snapshot restore path: places an order on the book *without* matching.
  // Only restore_book_image() uses this; inserting the image level by level in
  // FIFO order reproduces the original book byte for byte (and therefore the
  // same checksum). Returns false on duplicate id or capacity failure.
  bool restore_resting_order(Side side, Price price, Qty remaining, Seq seq,
                             OrderId id) noexcept {
    if (remaining == 0 || book_.find(id) != nullptr) return false;
    SubmitResult scratch{};
    scratch.id = id;
    if (!rest_order(side, price, remaining, remaining, seq, id, scratch)) return false;
    if (id >= next_id_) next_id_ = id + 1;
    return true;
  }

  // Read-only views.
  const OrderBook& book() const noexcept { return book_; }
  bool has_bid() const noexcept { return !book_.empty(Side::Buy); }
  bool has_ask() const noexcept { return !book_.empty(Side::Sell); }
  Price best_bid() const noexcept;
  Price best_ask() const noexcept;
  // Best ask - best bid in ticks; -1 when either side is empty.
  Price spread() const noexcept;
  std::size_t depth(Side side) const noexcept { return book_.depth(side); }
  const PriceLevel* level(Side side, std::size_t index) const noexcept {
    return book_.level(side, index);
  }
  const Order* find(OrderId id) const noexcept { return book_.find(id); }
  std::size_t active_orders() const noexcept { return book_.resting_orders(); }
  const Stats& stats() const noexcept { return stats_; }
  std::uint64_t trade_sequence() const noexcept { return trade_sequence_; }
  std::uint64_t trade_count() const noexcept { return trade_sequence_; }

  // Newest-first trade window into the rolling log (0 = most recent).
  std::size_t recent_trades(Trade* out, std::size_t max) const noexcept;

  // Canonical FIFO walk, used by snapshot encoding and the book visualizer.
  // level_order_at(side, level, 0) is the order next to match at that level.
  std::size_t level_order_count(Side side, std::size_t level_index) const noexcept;
  const Order* level_order_at(Side side, std::size_t level_index,
                             std::size_t k) const noexcept;

  // Canonical state checksum (price, totals, FIFO order, remaining quantities).
  std::uint64_t crc64() const noexcept;

 private:
  Qty match_incoming(Side taker_side, Price limit_price, bool is_market, Qty qty,
                     Seq seq, OrderId taker_id, SubmitResult& result) noexcept;
  bool rest_order(Side side, Price price, Qty qty, Qty remaining, Seq seq,
                  OrderId id, SubmitResult& result) noexcept;
  void record_trade(OrderId maker, OrderId taker, Price price, Qty qty, Seq seq,
                    Side taker_side) noexcept;
  OrderId allocate_id(OrderId requested) noexcept;

  OrderBook::Pool orders_;
  OrderBook book_;
  std::array<Trade, Limits::kMaxTradeLog> trades_{};
  std::size_t trade_head_ = 0;  // next write position
  std::uint64_t trade_sequence_ = 0;
  std::uint64_t trades_logged_ = 0;  // entries actually present in the ring
  OrderId next_id_ = 1;
  Stats stats_{};
};

}  // namespace ultralob
