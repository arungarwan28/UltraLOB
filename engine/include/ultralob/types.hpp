// UltraLOB — core scalar types.
//
// Prices are integer ticks (1 tick = 1 cent in the console). Floating point is
// never used inside the engine: it makes equality comparison, ordering and
// replay checksums platform dependent. Convert at the boundary only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ultralob {

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
using Price = std::int64_t;   // integer ticks
using Qty = std::uint64_t;
using Seq = std::uint64_t;    // event sequence number, monotonically increasing
using Timestamp = std::uint64_t;  // caller supplied nanoseconds (simulated clock)

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : std::uint8_t { Limit = 0, Market = 1 };
enum class OrderStatus : std::uint8_t {
  New = 0,
  PartiallyFilled = 1,
  Filled = 2,
  Cancelled = 3,
};

inline constexpr OrderId kInvalidId = 0;
inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

constexpr const char* side_name(Side s) noexcept {
  return s == Side::Buy ? "bid" : "ask";
}

// Boundary helpers: the console talks in currency units, the engine in ticks.
constexpr double price_to_units(Price p) noexcept {
  return static_cast<double>(p) / 100.0;
}

constexpr Price price_from_units(double units) noexcept {
  return static_cast<Price>(units * 100.0 + (units >= 0.0 ? 0.5 : -0.5));
}

}  // namespace ultralob
