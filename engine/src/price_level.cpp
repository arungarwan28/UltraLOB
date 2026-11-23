#include "ultralob/level_table.hpp"

#include <cstring>

namespace ultralob {

std::size_t LevelTable::index_of(Price price) const noexcept {
  std::size_t lo = 0;
  std::size_t hi = count_;
  while (lo < hi) {
    const std::size_t mid = lo + ((hi - lo) >> 1);
    const Price probe = levels_[mid].price;
    if (probe == price) return mid;
    if (before(probe, price)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return count_;
}

PriceLevel* LevelTable::find(Price price) noexcept {
  const std::size_t index = index_of(price);
  return index < count_ ? &levels_[index] : nullptr;
}

const PriceLevel* LevelTable::find(Price price) const noexcept {
  const std::size_t index = index_of(price);
  return index < count_ ? &levels_[index] : nullptr;
}

PriceLevel* LevelTable::insert(Price price) noexcept {
  if (count_ >= Limits::kMaxLevelsPerSide) return nullptr;

  // Upper bound in "better than" order: first level that does not sort before
  // `price`. Equal prices are impossible here (callers look up first).
  std::size_t lo = 0;
  std::size_t hi = count_;
  while (lo < hi) {
    const std::size_t mid = lo + ((hi - lo) >> 1);
    if (before(levels_[mid].price, price)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  if (lo < count_) {
    std::memmove(static_cast<void*>(&levels_[lo + 1]),
                 static_cast<const void*>(&levels_[lo]),
                 (count_ - lo) * sizeof(PriceLevel));
  }
  levels_[lo] = PriceLevel{};
  levels_[lo].price = price;
  ++count_;
  return &levels_[lo];
}

void LevelTable::erase_at(std::size_t index) noexcept {
  if (index >= count_) return;
  if (index + 1 < count_) {
    std::memmove(static_cast<void*>(&levels_[index]),
                 static_cast<const void*>(&levels_[index + 1]),
                 (count_ - index - 1) * sizeof(PriceLevel));
  }
  --count_;
  levels_[count_] = PriceLevel{};
}

}  // namespace ultralob
