#include "ultralob/matching_engine.hpp"

#include <algorithm>
#include <cstdio>

#include "ultralob/order_book.hpp"
#include "ultralob/price_level.hpp"

namespace ultralob {

const char* reject_reason_name(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::None: return "none";
    case RejectReason::InvalidQuantity: return "invalid_quantity";
    case RejectReason::InvalidPrice: return "invalid_price";
    case RejectReason::DuplicateOrderId: return "duplicate_order_id";
    case RejectReason::UnknownOrder: return "unknown_order";
    case RejectReason::OrderNotResting: return "order_not_resting";
    case RejectReason::BookFull: return "book_full";
    case RejectReason::OrderPoolExhausted: return "order_pool_exhausted";
    case RejectReason::IndexFull: return "index_full";
  }
  return "unknown";
}

MatchingEngine::MatchingEngine() noexcept : orders_(), book_(orders_) {}

MatchingEngine::~MatchingEngine() = default;

void MatchingEngine::reset() noexcept {
  book_.reset();  // clears levels, id index and the order pool
  trades_.fill(Trade{});
  trade_head_ = 0;
  trade_sequence_ = 0;
  trades_logged_ = 0;
  next_id_ = 1;
  stats_ = Stats{};
}

OrderId MatchingEngine::allocate_id(OrderId requested) noexcept {
  if (requested == kInvalidId) return next_id_++;
  if (requested >= next_id_) next_id_ = requested + 1;
  return requested;
}

Qty MatchingEngine::match_incoming(Side taker_side, Price limit_price, bool is_market,
                                   Qty qty, Seq seq, OrderId taker_id,
                                   SubmitResult& result) noexcept {
  const Side maker_side = opposite(taker_side);
  Qty remaining = qty;

  // Re-fetch the best level on every iteration: consuming the last order at a
  // level erases it, which shifts the level array.
  while (remaining > 0) {
    PriceLevel* level = book_.best_mut(maker_side);
    if (level == nullptr) break;
    if (!is_market) {
      const bool crosses = taker_side == Side::Buy ? level->price <= limit_price
                                                   : level->price >= limit_price;
      if (!crosses) break;
    }
    if (level->head == kNoIndex) break;  // defensive: empty level housekeeping

    Order& maker = orders_[level->head];
    const Qty fill = remaining < maker.remaining ? remaining : maker.remaining;
    maker.remaining -= fill;
    level->total_qty -= fill;
    remaining -= fill;

    record_trade(maker.id, taker_id, level->price, fill, seq, taker_side);
    ++stats_.executions;
    ++stats_.trades;
    ++result.trade_count;

    if (maker.remaining == 0) {
      maker.status = OrderStatus::Filled;
      book_.remove(maker);  // unlinks, erases empty level, frees the pool slot
      ++stats_.fills;
    } else {
      maker.status = OrderStatus::PartiallyFilled;
    }
  }
  return remaining;
}

void MatchingEngine::record_trade(OrderId maker, OrderId taker, Price price, Qty qty,
                                  Seq seq, Side taker_side) noexcept {
  Trade& trade = trades_[trade_head_];
  trade.id = ++trade_sequence_;
  trade.maker = maker;
  trade.taker = taker;
  trade.price = price;
  trade.qty = qty;
  trade.seq = seq;
  trade.taker_side = taker_side;
  trade_head_ = (trade_head_ + 1) % Limits::kMaxTradeLog;
  ++trades_logged_;

  stats_.traded_qty += qty;
  stats_.traded_notional += static_cast<std::uint64_t>(price) * qty;
}

bool MatchingEngine::rest_order(Side side, Price price, Qty qty, Qty remaining, Seq seq,
                                OrderId id, SubmitResult& result) noexcept {
  const std::uint32_t slot = orders_.acquire();
  if (slot == kNoIndex) {
    result.reason = RejectReason::OrderPoolExhausted;
    return false;
  }

  Order& order = orders_[slot];
  order.id = id;
  order.price = price;
  order.qty = qty;
  order.remaining = remaining;
  order.seq = seq;
  order.side = side;
  order.type = OrderType::Limit;
  order.status = OrderStatus::New;
  order.prev = kNoIndex;
  order.next = kNoIndex;
  order.slot = slot;

  if (!book_.add(order)) {
    orders_.release(slot);
    result.reason = RejectReason::BookFull;
    return false;
  }

  result.rested = true;
  return true;
}

SubmitResult MatchingEngine::submit_limit(Side side, Price price, Qty qty, Seq seq,
                                          OrderId id) noexcept {
  SubmitResult result{};
  result.id = id;

  if (qty == 0 || qty > Limits::kMaxOrderQty) {
    result.reason = RejectReason::InvalidQuantity;
    ++stats_.rejects;
    return result;
  }
  if (price < Limits::kMinPriceTicks || price > Limits::kMaxPriceTicks) {
    result.reason = RejectReason::InvalidPrice;
    ++stats_.rejects;
    return result;
  }
  if (id != kInvalidId && book_.find(id) != nullptr) {
    result.reason = RejectReason::DuplicateOrderId;
    ++stats_.rejects;
    return result;
  }

  ++stats_.submissions;
  result.accepted = true;
  result.id = allocate_id(id);

  const Qty remaining = match_incoming(side, price, false, qty, seq, result.id, result);
  result.filled = qty - remaining;
  result.remaining = remaining;

  if (remaining > 0 && !rest_order(side, price, qty, remaining, seq, result.id, result)) {
    // Capacity failure: the executed part stands, the remainder is refused.
    result.accepted = false;
    ++stats_.rejects;
  }
  return result;
}

SubmitResult MatchingEngine::submit_market(Side side, Qty qty, Seq seq,
                                           OrderId id) noexcept {
  SubmitResult result{};
  result.id = id;

  if (qty == 0 || qty > Limits::kMaxOrderQty) {
    result.reason = RejectReason::InvalidQuantity;
    ++stats_.rejects;
    return result;
  }
  if (id != kInvalidId && book_.find(id) != nullptr) {
    result.reason = RejectReason::DuplicateOrderId;
    ++stats_.rejects;
    return result;
  }

  ++stats_.submissions;
  result.accepted = true;
  result.id = allocate_id(id);

  // Market orders never rest: unfilled remainder is cancelled (IOC).
  const Qty remaining = match_incoming(side, 0, true, qty, seq, result.id, result);
  result.filled = qty - remaining;
  result.remaining = remaining;
  result.rested = false;
  return result;
}

SubmitResult MatchingEngine::cancel(OrderId id, Seq seq) noexcept {
  (void)seq;
  SubmitResult result{};
  result.id = id;

  Order* order = book_.find(id);
  if (order == nullptr) {
    result.reason = RejectReason::UnknownOrder;
    ++stats_.rejects;
    return result;
  }

  const Qty cancelled_qty = order->remaining;
  order->status = OrderStatus::Cancelled;
  book_.remove(*order);  // releases the pool slot

  result.accepted = true;
  result.remaining = cancelled_qty;
  ++stats_.cancels;
  return result;
}

SubmitResult MatchingEngine::modify(OrderId id, Price price, Qty qty, Seq seq) noexcept {
  SubmitResult result{};
  result.id = id;

  if (qty == 0 || qty > Limits::kMaxOrderQty) {
    result.reason = RejectReason::InvalidQuantity;
    ++stats_.rejects;
    return result;
  }
  if (price < Limits::kMinPriceTicks || price > Limits::kMaxPriceTicks) {
    result.reason = RejectReason::InvalidPrice;
    ++stats_.rejects;
    return result;
  }

  Order* order = book_.find(id);
  if (order == nullptr) {
    result.reason = RejectReason::UnknownOrder;
    ++stats_.rejects;
    return result;
  }

  ++stats_.modifies;
  const Side side = order->side;

  // Priority-preserving path: same price and a quantity reduction.
  if (price == order->price && qty <= order->remaining) {
    PriceLevel* level = book_.find_level(side, price);
    if (level != nullptr) {
      level->total_qty -= (order->remaining - qty);
      order->remaining = qty;
      order->qty = qty;
      result.accepted = true;
      result.rested = true;
      result.remaining = qty;
      return result;
    }
  }

  // Any other modify loses queue priority: remove and re-enter, matching as a
  // taker if the new price crosses the book.
  book_.remove(*order);
  result = submit_limit(side, price, qty, seq, id);
  return result;
}

Price MatchingEngine::best_bid() const noexcept {
  const PriceLevel* level = book_.best(Side::Buy);
  return level == nullptr ? 0 : level->price;
}

Price MatchingEngine::best_ask() const noexcept {
  const PriceLevel* level = book_.best(Side::Sell);
  return level == nullptr ? 0 : level->price;
}

Price MatchingEngine::spread() const noexcept {
  if (!has_bid() || !has_ask()) return -1;
  return best_ask() - best_bid();
}

std::size_t MatchingEngine::recent_trades(Trade* out, std::size_t max) const noexcept {
  if (out == nullptr || max == 0) return 0;
  const std::size_t logged = trades_logged_ < Limits::kMaxTradeLog
                                 ? static_cast<std::size_t>(trades_logged_)
                                 : Limits::kMaxTradeLog;
  const std::size_t count = logged < max ? logged : max;
  std::size_t pos = trade_head_;
  for (std::size_t i = 0; i < count; ++i) {
    pos = (pos == 0 ? Limits::kMaxTradeLog : pos) - 1;
    out[i] = trades_[pos];
  }
  return count;
}

std::size_t MatchingEngine::level_order_count(Side side,
                                             std::size_t level_index) const noexcept {
  const PriceLevel* level = book_.level(side, level_index);
  return level == nullptr ? 0 : level->order_count;
}

const Order* MatchingEngine::level_order_at(Side side, std::size_t level_index,
                                           std::size_t k) const noexcept {
  const PriceLevel* level = book_.level(side, level_index);
  if (level == nullptr) return nullptr;
  std::uint32_t slot = level->head;
  while (slot != kNoIndex && k > 0) {
    slot = orders_[slot].next;
    --k;
  }
  if (slot == kNoIndex) return nullptr;
  return &orders_[slot];
}

std::uint64_t MatchingEngine::crc64() const noexcept {
  std::uint64_t crc = kCrc64Init;
  const Side sides[2] = {Side::Buy, Side::Sell};
  for (const Side side : sides) {
    crc = crc64_update_u64(crc, static_cast<std::uint64_t>(depth(side)));
    for (std::size_t i = 0; i < depth(side); ++i) {
      const PriceLevel* level = book_.level(side, i);
      crc = crc64_update_u64(crc, static_cast<std::uint64_t>(level->price));
      crc = crc64_update_u64(crc, level->total_qty);
      crc = crc64_update_u64(crc, level->order_count);
      for (std::uint32_t slot = level->head; slot != kNoIndex;
           slot = orders_[slot].next) {
        const Order& order = orders_[slot];
        crc = crc64_update_u64(crc, order.id);
        crc = crc64_update_u64(crc, order.remaining);
      }
    }
  }
  // The checksum covers book state only: levels, aggregate quantities, FIFO
  // order and remaining quantities. Trades are a derived consequence of state
  // transitions, and counting them here would make a restored book image
  // unverifiable (see docs/design-decisions.md).
  crc = crc64_update_u64(crc, static_cast<std::uint64_t>(active_orders()));
  return crc;
}

}  // namespace ultralob
