// UltraLOB — baseline matching engine (deliberately naive).
//
// This is the A/B reference for benchmarks/compare_benchmarks.cpp. It implements
// the *same* trading semantics as the optimized engine:
//
//   * price-time priority, best price first, FIFO inside a level
//   * limit orders match what crosses, then rest
//   * market orders are IOC (unfilled remainder cancelled)
//   * modify keeps priority only when price is unchanged and quantity does not
//     increase; anything else re-enters at the tail and may trade
//   * rejects on zero/too-large quantity, out-of-range price, unknown id,
//     duplicate id, and capacity exhaustion
//
// and the *same* canonical state checksum, so the harness can prove the two
// implementations agree rather than assuming it.
//
// The implementation is what a competent engineer writes first: std::map for
// price levels, std::list for FIFO queues, std::unordered_map for the id index,
// std::vector for the trade tape. Every insert allocates, every cancel frees,
// every level lookup walks a red-black tree. It is not tuned, and that is the
// point of the comparison.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "ultralob/config.hpp"
#include "ultralob/crc64.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/types.hpp"

namespace ultralob {
namespace baseline {

class Engine {
 public:
  struct Counters {
    std::uint64_t submissions = 0;
    std::uint64_t rejects = 0;
    std::uint64_t cancels = 0;
    std::uint64_t modifies = 0;
    std::uint64_t fills = 0;
    std::uint64_t executions = 0;
  };

  void reset() {
    bids_.clear();
    asks_.clear();
    index_.clear();
    trades_.clear();
    trade_sequence_ = 0;
    total_trades_ = 0;
    next_id_ = 1;
    counters_ = Counters{};
  }

  // --- queries -------------------------------------------------------------
  bool has_bid() const noexcept { return !bids_.empty(); }
  bool has_ask() const noexcept { return !asks_.empty(); }
  Price best_bid() const noexcept { return bids_.empty() ? 0 : bids_.begin()->first; }
  Price best_ask() const noexcept { return asks_.empty() ? 0 : asks_.begin()->first; }
  Price spread() const noexcept {
    if (!has_bid() || !has_ask()) return -1;
    return best_ask() - best_bid();
  }
  std::size_t depth(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
  }
  std::size_t active_orders() const noexcept { return index_.size(); }
  std::uint64_t trade_count() const noexcept { return total_trades_; }
  const Counters& counters() const noexcept { return counters_; }

  Price level_price(Side side, std::size_t index) const noexcept {
    if (side == Side::Buy) {
      if (index >= bids_.size()) return 0;
      auto it = bids_.begin();
      std::advance(it, static_cast<long>(index));
      return it->first;
    }
    if (index >= asks_.size()) return 0;
    auto it = asks_.begin();
    std::advance(it, static_cast<long>(index));
    return it->first;
  }

  Qty level_qty(Side side, std::size_t index) const noexcept {
    if (side == Side::Buy) {
      if (index >= bids_.size()) return 0;
      auto it = bids_.begin();
      std::advance(it, static_cast<long>(index));
      return it->second.total_qty;
    }
    if (index >= asks_.size()) return 0;
    auto it = asks_.begin();
    std::advance(it, static_cast<long>(index));
    return it->second.total_qty;
  }

  // Newest first, like MatchingEngine::recent_trades.
  std::size_t recent_trades(Trade* out, std::size_t max) const noexcept {
    if (out == nullptr || max == 0) return 0;
    const std::size_t count = std::min(max, trades_.size());
    for (std::size_t i = 0; i < count; ++i) out[i] = trades_[trades_.size() - 1 - i];
    return count;
  }

  // Byte-for-byte the same canonical walk as MatchingEngine::crc64().
  std::uint64_t crc64() const noexcept {
    std::uint64_t crc = kCrc64Init;
    crc = crc64_update_u64(crc, static_cast<std::uint64_t>(bids_.size()));
    for (const auto& entry : bids_) {
      crc = crc64_update_u64(crc, static_cast<std::uint64_t>(entry.first));
      crc = crc64_update_u64(crc, entry.second.total_qty);
      crc = crc64_update_u64(crc, entry.second.order_count);
      for (const RestingOrder& order : entry.second.fifo) {
        crc = crc64_update_u64(crc, order.id);
        crc = crc64_update_u64(crc, order.remaining);
      }
    }
    crc = crc64_update_u64(crc, static_cast<std::uint64_t>(asks_.size()));
    for (const auto& entry : asks_) {
      crc = crc64_update_u64(crc, static_cast<std::uint64_t>(entry.first));
      crc = crc64_update_u64(crc, entry.second.total_qty);
      crc = crc64_update_u64(crc, entry.second.order_count);
      for (const RestingOrder& order : entry.second.fifo) {
        crc = crc64_update_u64(crc, order.id);
        crc = crc64_update_u64(crc, order.remaining);
      }
    }
    return crc64_update_u64(crc, static_cast<std::uint64_t>(active_orders()));
  }

  // --- mutators ------------------------------------------------------------
  SubmitResult submit_limit(Side side, Price price, Qty qty, Seq seq,
                            OrderId id = kInvalidId) {
    SubmitResult result{};
    result.id = id;

    if (qty == 0 || qty > Limits::kMaxOrderQty) {
      result.reason = RejectReason::InvalidQuantity;
      ++counters_.rejects;
      return result;
    }
    if (price < Limits::kMinPriceTicks || price > Limits::kMaxPriceTicks) {
      result.reason = RejectReason::InvalidPrice;
      ++counters_.rejects;
      return result;
    }
    if (id != kInvalidId && index_.find(id) != index_.end()) {
      result.reason = RejectReason::DuplicateOrderId;
      ++counters_.rejects;
      return result;
    }

    ++counters_.submissions;
    result.accepted = true;
    result.id = allocate_id(id);

    Qty remaining = qty;
    if (side == Side::Buy) {
      match(asks_, side, price, false, remaining, seq, result.id, result);
    } else {
      match(bids_, side, price, false, remaining, seq, result.id, result);
    }
    result.filled = qty - remaining;
    result.remaining = remaining;

    if (remaining > 0 &&
        !rest(side, price, qty, remaining, seq, result.id, result)) {
      // Capacity failure: executed part stands, remainder refused.
      result.accepted = false;
      ++counters_.rejects;
    }
    return result;
  }

  SubmitResult submit_market(Side side, Qty qty, Seq seq, OrderId id = kInvalidId) {
    SubmitResult result{};
    result.id = id;

    if (qty == 0 || qty > Limits::kMaxOrderQty) {
      result.reason = RejectReason::InvalidQuantity;
      ++counters_.rejects;
      return result;
    }
    if (id != kInvalidId && index_.find(id) != index_.end()) {
      result.reason = RejectReason::DuplicateOrderId;
      ++counters_.rejects;
      return result;
    }

    ++counters_.submissions;
    result.accepted = true;
    result.id = allocate_id(id);

    Qty remaining = qty;
    if (side == Side::Buy) {
      match(asks_, side, 0, true, remaining, seq, result.id, result);
    } else {
      match(bids_, side, 0, true, remaining, seq, result.id, result);
    }
    result.filled = qty - remaining;
    result.remaining = remaining;
    result.rested = false;
    return result;
  }

  SubmitResult cancel(OrderId id, Seq seq) {
    (void)seq;
    SubmitResult result{};
    result.id = id;

    auto found = index_.find(id);
    if (found == index_.end()) {
      result.reason = RejectReason::UnknownOrder;
      ++counters_.rejects;
      return result;
    }

    const Qty cancelled = found->second.iterator->remaining;
    remove(found);
    result.accepted = true;
    result.remaining = cancelled;
    ++counters_.cancels;
    return result;
  }

  SubmitResult modify(OrderId id, Price price, Qty qty, Seq seq) {
    SubmitResult result{};
    result.id = id;

    if (qty == 0 || qty > Limits::kMaxOrderQty) {
      result.reason = RejectReason::InvalidQuantity;
      ++counters_.rejects;
      return result;
    }
    if (price < Limits::kMinPriceTicks || price > Limits::kMaxPriceTicks) {
      result.reason = RejectReason::InvalidPrice;
      ++counters_.rejects;
      return result;
    }

    auto found = index_.find(id);
    if (found == index_.end()) {
      result.reason = RejectReason::UnknownOrder;
      ++counters_.rejects;
      return result;
    }

    ++counters_.modifies;
    const Side side = found->second.side;

    // Priority-preserving path: same price, quantity not increased.
    if (price == found->second.price && qty <= found->second.iterator->remaining) {
      RestingOrder& order = *found->second.iterator;
      Level& level = find_level(side, price);
      level.total_qty -= (order.remaining - qty);
      order.remaining = qty;
      order.qty = qty;
      result.accepted = true;
      result.rested = true;
      result.remaining = qty;
      return result;
    }

    remove(found);
    return submit_limit(side, price, qty, seq, id);
  }

 private:
  struct RestingOrder {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;
    Qty remaining = 0;
    Seq seq = 0;
    Side side = Side::Buy;
  };

  using Queue = std::list<RestingOrder>;

  struct Level {
    Qty total_qty = 0;
    std::uint32_t order_count = 0;
    Queue fifo;
  };

  struct Location {
    Side side = Side::Buy;
    Price price = 0;
    Queue::iterator iterator{};
  };

  using BidMap = std::map<Price, Level, std::greater<Price>>;
  using AskMap = std::map<Price, Level, std::less<Price>>;

  OrderId allocate_id(OrderId requested) {
    if (requested == kInvalidId) return next_id_++;
    if (requested >= next_id_) next_id_ = requested + 1;
    return requested;
  }

  Level& find_level(Side side, Price price) {
    return side == Side::Buy ? bids_.find(price)->second : asks_.find(price)->second;
  }

  template <typename Map>
  void match(Map& levels, Side taker_side, Price limit_price, bool is_market,
             Qty& remaining, Seq seq, OrderId taker_id, SubmitResult& result) {
    auto it = levels.begin();
    while (remaining > 0 && it != levels.end()) {
      if (!is_market) {
        const bool crosses = taker_side == Side::Buy ? it->first <= limit_price
                                                     : it->first >= limit_price;
        if (!crosses) break;
      }
      Level& level = it->second;
      while (remaining > 0 && !level.fifo.empty()) {
        RestingOrder& maker = level.fifo.front();
        const Qty fill = std::min(remaining, maker.remaining);
        maker.remaining -= fill;
        level.total_qty -= fill;
        remaining -= fill;

        Trade trade{};
        trade.id = ++trade_sequence_;
        trade.maker = maker.id;
        trade.taker = taker_id;
        trade.price = it->first;
        trade.qty = fill;
        trade.seq = seq;
        trade.taker_side = taker_side;
        trades_.push_back(trade);
        ++total_trades_;
        ++result.trade_count;
        ++counters_.executions;

        if (maker.remaining == 0) {
          index_.erase(maker.id);
          level.fifo.pop_front();
          --level.order_count;
          ++counters_.fills;
        }
      }
      if (level.fifo.empty()) {
        it = levels.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool rest(Side side, Price price, Qty qty, Qty remaining, Seq seq, OrderId id,
            SubmitResult& result) {
    if (index_.size() >= Limits::kMaxOrders) {
      result.reason = RejectReason::OrderPoolExhausted;
      return false;
    }

    RestingOrder order{};
    order.id = id;
    order.price = price;
    order.qty = qty;
    order.remaining = remaining;
    order.seq = seq;
    order.side = side;

    if (side == Side::Buy) {
      auto it = bids_.find(price);
      if (it == bids_.end()) {
        if (bids_.size() >= Limits::kMaxLevelsPerSide) {
          result.reason = RejectReason::BookFull;
          return false;
        }
        it = bids_.emplace(price, Level{}).first;
      }
      it->second.fifo.push_back(order);
      it->second.total_qty += remaining;
      ++it->second.order_count;
      index_.emplace(id, Location{side, price, std::prev(it->second.fifo.end())});
    } else {
      auto it = asks_.find(price);
      if (it == asks_.end()) {
        if (asks_.size() >= Limits::kMaxLevelsPerSide) {
          result.reason = RejectReason::BookFull;
          return false;
        }
        it = asks_.emplace(price, Level{}).first;
      }
      it->second.fifo.push_back(order);
      it->second.total_qty += remaining;
      ++it->second.order_count;
      index_.emplace(id, Location{side, price, std::prev(it->second.fifo.end())});
    }

    result.rested = true;
    return true;
  }

  void remove(std::unordered_map<OrderId, Location>::iterator found) {
    Location location = found->second;
    Qty remaining = location.iterator->remaining;
    index_.erase(found);

    if (location.side == Side::Buy) {
      auto level_it = bids_.find(location.price);
      Level& level = level_it->second;
      level.total_qty -= remaining;
      level.fifo.erase(location.iterator);
      --level.order_count;
      if (level.fifo.empty()) bids_.erase(level_it);
    } else {
      auto level_it = asks_.find(location.price);
      Level& level = level_it->second;
      level.total_qty -= remaining;
      level.fifo.erase(location.iterator);
      --level.order_count;
      if (level.fifo.empty()) asks_.erase(level_it);
    }
  }

  BidMap bids_{};
  AskMap asks_{};
  std::unordered_map<OrderId, Location> index_{};
  std::vector<Trade> trades_{};
  std::uint64_t trade_sequence_ = 0;
  std::uint64_t total_trades_ = 0;
  OrderId next_id_ = 1;
  Counters counters_{};
};

}  // namespace baseline
}  // namespace ultralob
