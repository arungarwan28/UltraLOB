// UltraLOB — deterministic synthetic market feed.
//
// The generator is the single source of market traffic for the native
// benchmark, the browser benchmark, the replay lab and the Python parity test.
// It uses integer arithmetic only (xorshift64*) so a (seed, workload) pair
// produces byte-identical event streams on every platform.
//
// This is SIMULATED data. It models plausible market microstructure — no
// real venue feed, no real instrument.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ultralob/event.hpp"
#include "ultralob/types.hpp"

namespace ultralob {

enum class Workload : std::uint32_t {
  Balanced = 0,     // mixed flow: adds, cancels, modifies, some market orders
  CancelHeavy = 1,  // churn: most traffic is cancels/replaces
  Burst = 2,        // tight spread and larger clips (a 10x rate proxy)
};

const char* workload_name(Workload workload) noexcept;

struct FeedConfig {
  std::uint64_t seed = 0x5EED5EEDull;
  Workload workload = Workload::Balanced;
  Price mid_price = 10000;        // 100.00 in ticks
  std::uint32_t half_spread = 6;  // quoting distance in ticks
  Qty qty_min = 1;
  Qty qty_max = 200;
  std::uint32_t pct_market = 6;   // percent of events that are market orders
  std::uint32_t pct_cancel = 26;
  std::uint32_t pct_modify = 9;
  std::uint32_t mid_walk = 2;     // max ticks the mid drifts per event
};

FeedConfig workload_config(Workload workload, std::uint64_t seed) noexcept;

class FeedGenerator {
 public:
  static constexpr std::size_t kLiveWindow = 4096;

  explicit FeedGenerator(FeedConfig config) noexcept;

  // Next event in the stream (sequence starts at 1 and never skips).
  Event next() noexcept;

  Seq next_seq() const noexcept { return seq_ + 1; }
  const FeedConfig& config() const noexcept { return config_; }
  std::size_t live_orders() const noexcept { return live_count_; }

  // Peek without consuming (cheap: generator state is a handful of integers).
  Event peek() const noexcept {
    FeedGenerator copy = *this;
    return copy.next();
  }

 private:
  struct Rng {
    std::uint64_t state;
    std::uint64_t next() noexcept;
    std::uint32_t below(std::uint32_t bound) noexcept;
    std::uint64_t qty(Qty lo, Qty hi) noexcept;
  };

  Event build_new(Rng& rng) noexcept;
  Event build_market(Rng& rng) noexcept;
  Event build_cancel(Rng& rng) noexcept;
  Event build_modify(Rng& rng) noexcept;
  void remember_order(OrderId id) noexcept;
  OrderId pick_live(Rng& rng) noexcept;

  FeedConfig config_;
  Rng rng_;
  Seq seq_ = 0;
  Price mid_ = 0;
  OrderId next_order_id_ = 1'000'000;  // high base: never collides with UI auto ids
  std::array<OrderId, kLiveWindow> live_{};
  std::size_t live_count_ = 0;
};

}  // namespace ultralob
