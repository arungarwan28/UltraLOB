#include "ultralob/order_book.hpp"

namespace ultralob {

void OrderBook::reset() noexcept {
  bids_.clear();
  asks_.clear();
  index_.clear();
  pool().reset();
}

bool OrderBook::add(Order& handle) noexcept {
  if (!pool().is_live(handle.slot)) return false;
  const std::uint32_t slot = handle.slot;
  Order& order = pool()[slot];

  // The book owns its resting records: adopt the caller's values into the pool
  // slot. Without this the pool record's side/id/price stay unset (only
  // MatchingEngine::rest_order used to fill them), and a later remove() would
  // read the wrong side and unlink nothing.
  order.id = handle.id;
  order.price = handle.price;
  order.qty = handle.qty;
  order.remaining = handle.remaining;
  order.seq = handle.seq;
  order.side = handle.side;
  order.type = handle.type;
  order.slot = slot;

  LevelTable& levels = table(order.side);

  PriceLevel* level = levels.find(order.price);
  const bool created_level = level == nullptr;
  if (created_level) {
    level = levels.insert(order.price);
    if (level == nullptr) return false;  // level table full for this side
  }

  if (!index_.insert(order.id, order.slot)) {
    // Roll the level back if this call created it. Leaving an empty level at
    // the touch would make best_bid/best_ask report a price with no orders and
    // would stop matching on that side (the level has no FIFO head).
    if (created_level) levels.erase_at(levels.index_of(order.price));
    return false;  // duplicate id / index full
  }

  order.prev = level->tail;
  order.next = kNoIndex;
  if (level->tail != kNoIndex) {
    pool()[level->tail].next = order.slot;
  } else {
    level->head = order.slot;
  }
  level->tail = order.slot;
  level->total_qty += order.remaining;
  ++level->order_count;
  order.status = order.remaining == order.qty ? OrderStatus::New
                                              : OrderStatus::PartiallyFilled;
  return true;
}

void OrderBook::remove(Order& handle) noexcept {
  // `handle` is only a pointer to a pool slot: its fields may be a stale copy
  // held by the caller (they are not kept in sync when a later order is linked
  // after it). Unlinking therefore reads the authoritative pool record, so a
  // stale handle cannot corrupt the FIFO.
  if (!pool().is_live(handle.slot)) return;
  const std::uint32_t slot = handle.slot;
  Order& order = pool()[slot];
  LevelTable& levels = table(order.side);

  PriceLevel* level = levels.find(order.price);
  if (level != nullptr) {
    if (order.prev != kNoIndex) {
      pool()[order.prev].next = order.next;
    } else {
      level->head = order.next;
    }
    if (order.next != kNoIndex) {
      pool()[order.next].prev = order.prev;
    } else {
      level->tail = order.prev;
    }
    level->total_qty -= (order.remaining < level->total_qty) ? order.remaining
                                                             : level->total_qty;
    if (level->order_count > 0) --level->order_count;
    if (level->order_count == 0) levels.erase_at(levels.index_of(order.price));
  }

  

  index_.erase(order.id);
  order.prev = kNoIndex;
  order.next = kNoIndex;
  order.remaining = 0;
  pool().release(slot);
}

const PriceLevel* OrderBook::best(Side side) const noexcept {
  const LevelTable& levels = table(side);
  return levels.size() == 0 ? nullptr : &levels[0];
}

PriceLevel* OrderBook::best_mut(Side side) noexcept {
  LevelTable& levels = table(side);
  return levels.size() == 0 ? nullptr : &levels[0];
}

const PriceLevel* OrderBook::level(Side side, std::size_t index) const noexcept {
  const LevelTable& levels = table(side);
  return index < levels.size() ? &levels[index] : nullptr;
}

PriceLevel* OrderBook::find_level(Side side, Price price) noexcept {
  return table(side).find(price);
}

const PriceLevel* OrderBook::find_level(Side side, Price price) const noexcept {
  return table(side).find(price);
}

Qty OrderBook::qty_at(Side side, Price price) const noexcept {
  const PriceLevel* level = table(side).find(price);
  return level == nullptr ? 0 : level->total_qty;
}

const Order* OrderBook::find(OrderId id) const noexcept {
  const std::uint32_t slot = index_.find(id);
  if (slot == kNoIndex || !pool().is_live(slot)) return nullptr;
  return &pool()[slot];
}

Order* OrderBook::find(OrderId id) noexcept {
  const std::uint32_t slot = index_.find(id);
  if (slot == kNoIndex || !pool().is_live(slot)) return nullptr;
  return &pool()[slot];
}

}  // namespace ultralob
