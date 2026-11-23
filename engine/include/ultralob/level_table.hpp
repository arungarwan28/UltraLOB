// UltraLOB — sorted price level table.
//
// Design note (see docs/design-decisions.md): levels are kept in a sorted
// fixed-capacity array instead of a std::map. That trades O(log n) level insert
// for O(n) memmove, but removes all heap traffic from the book, keeps the
// best price at index 0 (perfect for matching, depth export and checksums) and
// makes the entire book a single contiguous scan. Realistic books are a few
// hundred levels, so n stays small.
#pragma once

#include <array>
#include <cstddef>

#include "ultralob/config.hpp"
#include "ultralob/price_level.hpp"
#include "ultralob/types.hpp"

namespace ultralob {

class LevelTable {
 public:
  explicit LevelTable(Side side) noexcept : side_(side) {}

  void clear() noexcept { count_ = 0; }
  std::size_t size() const noexcept { return count_; }
  bool full() const noexcept { return count_ >= Limits::kMaxLevelsPerSide; }

  PriceLevel& operator[](std::size_t i) noexcept { return levels_[i]; }
  const PriceLevel& operator[](std::size_t i) const noexcept { return levels_[i]; }

  // Index of `price`, or size() when the price is not on the book.
  std::size_t index_of(Price price) const noexcept;

  PriceLevel* find(Price price) noexcept;
  const PriceLevel* find(Price price) const noexcept;

  // Inserts an empty level in sorted position (best-first). nullptr when full.
  PriceLevel* insert(Price price) noexcept;

  void erase_at(std::size_t index) noexcept;

 private:
  // Sort order: bids descending, asks ascending.
  bool before(Price a, Price b) const noexcept {
    return side_ == Side::Buy ? a > b : a < b;
  }

  std::array<PriceLevel, Limits::kMaxLevelsPerSide> levels_{};
  std::size_t count_ = 0;
  Side side_;
};

}  // namespace ultralob
