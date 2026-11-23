#include "ultralob/feed_generator.hpp"

#include "ultralob/config.hpp"

namespace ultralob {

const char* workload_name(Workload workload) noexcept {
  switch (workload) {
    case Workload::Balanced: return "balanced";
    case Workload::CancelHeavy: return "cancel_heavy";
    case Workload::Burst: return "burst";
  }
  return "unknown";
}

FeedConfig workload_config(Workload workload, std::uint64_t seed) noexcept {
  FeedConfig config{};
  config.seed = seed;
  config.workload = workload;
  switch (workload) {
    case Workload::Balanced:
      break;
    case Workload::CancelHeavy:
      config.pct_market = 3;
      config.pct_cancel = 55;
      config.pct_modify = 22;
      config.half_spread = 4;
      config.qty_max = 120;
      break;
    case Workload::Burst:
      // Tighter quotes and much larger clips: a proxy for a 10x message-rate
      // burst where cancellations and marketable flow compete for liquidity.
      config.pct_market = 14;
      config.pct_cancel = 18;
      config.pct_modify = 8;
      config.half_spread = 2;
      config.qty_max = 900;
      config.mid_walk = 4;
      break;
  }
  return config;
}

FeedGenerator::FeedGenerator(FeedConfig config) noexcept : config_(config) {
  rng_.state = config_.seed == 0 ? 0x9E3779B97F4A7C15ull : config_.seed;
  mid_ = static_cast<Price>(config_.mid_price);
  if (mid_ < 100) mid_ = 100;
  if (config_.qty_max < config_.qty_min) config_.qty_max = config_.qty_min;
}

std::uint64_t FeedGenerator::Rng::next() noexcept {
  std::uint64_t x = state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  state = x;
  return x;
}

std::uint32_t FeedGenerator::Rng::below(std::uint32_t bound) noexcept {
  if (bound == 0) return 0;
  return static_cast<std::uint32_t>(next() % bound);
}

std::uint64_t FeedGenerator::Rng::qty(Qty lo, Qty hi) noexcept {
  if (hi <= lo) return lo;
  return lo + next() % (hi - lo + 1);
}

void FeedGenerator::remember_order(OrderId id) noexcept {
  if (live_count_ < kLiveWindow) {
    live_[live_count_++] = id;
  } else {
    // Bounded window: forget the oldest tracked id (its order simply rests
    // forever, which is realistic and keeps the generator O(1)).
    for (std::size_t i = 1; i < kLiveWindow; ++i) live_[i - 1] = live_[i];
    live_[kLiveWindow - 1] = id;
  }
}

OrderId FeedGenerator::pick_live(Rng& rng) noexcept {
  if (live_count_ == 0) return kInvalidId;
  // Pick from the most recent adds: older ones are likelier to have been filled
  // already, and cancelling a filled order only produces a realistic-but-noisy
  // engine reject. This keeps the synthetic flow closer to a live venue.
  const std::size_t window = live_count_ < 96 ? live_count_ : 96;
  const std::size_t index = live_count_ - window + rng.below(static_cast<std::uint32_t>(window));
  const OrderId id = live_[index];
  live_[index] = live_[live_count_ - 1];
  --live_count_;
  return id;
}

Event FeedGenerator::build_new(Rng& rng) noexcept {
  Event event{};
  event.type = EventType::NewOrder;
  event.order_type = OrderType::Limit;
  event.side = rng.below(2) == 0 ? Side::Buy : Side::Sell;

  const std::uint32_t spread = config_.half_spread == 0 ? 1 : config_.half_spread;
  const std::uint32_t offset = rng.below(spread + 1);
  const Price distance = static_cast<Price>(offset);
  event.price = event.side == Side::Buy ? mid_ - distance : mid_ + distance;
  if (event.price < Limits::kMinPriceTicks) event.price = Limits::kMinPriceTicks;
  if (event.price > Limits::kMaxPriceTicks) event.price = Limits::kMaxPriceTicks;

  event.qty = rng.qty(config_.qty_min, config_.qty_max);
  event.order_id = next_order_id_++;
  // Only track deep quotes (the far edge of the configured spread). Quotes at
  // or near the touch are the ones market flow consumes first, so cancelling
  // them would mostly produce realistic-but-noisy engine rejects; venues see
  // stale cancels, but a benchmark should not be dominated by them.
  if (offset >= config_.half_spread) remember_order(event.order_id);
  return event;
}

Event FeedGenerator::build_market(Rng& rng) noexcept {
  Event event{};
  event.type = EventType::NewOrder;
  event.order_type = OrderType::Market;
  event.side = rng.below(2) == 0 ? Side::Buy : Side::Sell;
  // Aggressive flow arrives in smaller clips than passive quotes.
  const Qty market_cap = config_.qty_max / 4;
  event.qty = rng.qty(config_.qty_min, market_cap < config_.qty_min ? config_.qty_min : market_cap);
  event.order_id = next_order_id_++;
  return event;
}

Event FeedGenerator::build_cancel(Rng& rng) noexcept {
  Event event{};
  event.type = EventType::CancelOrder;
  event.order_id = pick_live(rng);
  return event;
}

Event FeedGenerator::build_modify(Rng& rng) noexcept {
  Event event{};
  event.type = EventType::ModifyOrder;
  event.order_id = pick_live(rng);
  event.side = rng.below(2) == 0 ? Side::Buy : Side::Sell;

  const std::uint32_t spread = config_.half_spread == 0 ? 1 : config_.half_spread;
  const Price distance = static_cast<Price>(rng.below(spread + 1));
  event.price = event.side == Side::Buy ? mid_ - distance : mid_ + distance;
  if (event.price < Limits::kMinPriceTicks) event.price = Limits::kMinPriceTicks;
  event.qty = rng.qty(config_.qty_min, config_.qty_max);
  remember_order(event.order_id);
  return event;
}

Event FeedGenerator::next() noexcept {
  ++seq_;

  if (config_.mid_walk > 0) {
    const Price span = static_cast<Price>(config_.mid_walk);
    const Price drift = static_cast<Price>(rng_.below(config_.mid_walk * 2 + 1)) - span;
    mid_ += drift;
    const Price floor = 100 + static_cast<Price>(config_.half_spread) + 1;
    if (mid_ < floor) mid_ = floor;
    const Price ceiling = Limits::kMaxPriceTicks / 2;
    if (mid_ > ceiling) mid_ = ceiling;
  }

  const std::uint32_t roll = rng_.below(100);
  const std::uint32_t market_cut = config_.pct_market;
  const std::uint32_t cancel_cut = market_cut + config_.pct_cancel;
  const std::uint32_t modify_cut = cancel_cut + config_.pct_modify;

  Event event{};
  if (roll < market_cut) {
    event = build_market(rng_);
  } else if (roll < cancel_cut && live_count_ > 0) {
    event = build_cancel(rng_);
  } else if (roll < modify_cut && live_count_ > 0) {
    event = build_modify(rng_);
  } else {
    event = build_new(rng_);
  }

  event.seq = seq_;
  event.id = seq_;  // one event id per stream position for generated feeds
  event.ts = seq_ * 1000ull;  // 1 us of simulated spacing
  if (event.order_id == kInvalidId) {
    // No live order to cancel/modify: fall back to a fresh limit order so the
    // stream stays valid and deterministic.
    event = build_new(rng_);
    event.seq = seq_;
    event.id = seq_;
    event.ts = seq_ * 1000ull;
  }
  return event;
}

}  // namespace ultralob
