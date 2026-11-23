// UltraLOB — compile-time capacities.
//
// Every hot-path structure is fixed capacity so the engine performs no heap
// allocation after construction. Capacities are sized for an interactive
// simulator (and stay inside a browser-friendly WASM linear memory); the
// native benchmark reports against the same limits.
#pragma once

#include <cstddef>

#include "ultralob/types.hpp"

namespace ultralob {

struct Limits {
  // Live orders held in the index-based pool.
  static constexpr std::size_t kMaxOrders = 1u << 17;  // 131,072
  // Price levels tracked per side (sorted array, zero allocation).
  static constexpr std::size_t kMaxLevelsPerSide = 4096;
  // Rolling trade log exposed to the UI / tests.
  static constexpr std::size_t kMaxTradeLog = 1u << 16;  // 65,536
  // Levels included in a book snapshot / checksum comparison view.
  static constexpr std::size_t kSnapshotLevels = 10;
  // Direct-mapped window of recently applied event ids (duplicate detection).
  static constexpr std::size_t kRecentEventIds = 4096;
  // Bounded reorder buffer for out-of-order delivery (gap policy: buffer).
  static constexpr std::size_t kReorderBuffer = 512;
  // Guard rails on order input.
  static constexpr Price kMinPriceTicks = 1;
  static constexpr Price kMaxPriceTicks = 100'000'000;  // $1,000,000.00
  static constexpr Qty kMaxOrderQty = 1'000'000'000;
};

}  // namespace ultralob
