#include "ultralob/order_index.hpp"

#include <algorithm>

namespace ultralob {

void OrderIndex::clear() noexcept {
  keys_.fill(0);
  size_ = 0;
}

bool OrderIndex::insert(OrderId id, std::uint32_t slot) noexcept {
  if (id == kInvalidId) return false;
  // Keep load factor <= 50% so probe chains stay short.
  if ((size_ + 1) * 2 > kCapacity) return false;

  std::size_t i = static_cast<std::size_t>(hash(id)) & kMask;
  while (keys_[i] != 0) {
    if (keys_[i] == id) return false;  // already indexed
    i = (i + 1) & kMask;
  }
  keys_[i] = id;
  slots_[i] = slot;
  ++size_;
  return true;
}

bool OrderIndex::erase(OrderId id) noexcept {
  if (id == kInvalidId) return false;

  std::size_t i = static_cast<std::size_t>(hash(id)) & kMask;
  std::size_t hole = kCapacity;
  while (keys_[i] != 0) {
    if (keys_[i] == id) {
      hole = i;
      break;
    }
    i = (i + 1) & kMask;
  }
  if (hole == kCapacity) return false;

  // Backward-shift deletion: pull later members of the probe chain into the
  // hole while keeping their search invariant intact. No tombstones, so heavy
  // add/cancel churn never degrades lookup cost.
  std::size_t j = hole;
  for (;;) {
    j = (j + 1) & kMask;
    if (keys_[j] == 0) break;
    const std::size_t ideal = static_cast<std::size_t>(hash(keys_[j])) & kMask;
    const bool outside_range =
        (j > hole) ? (ideal <= hole || ideal > j) : (ideal <= hole && ideal > j);
    if (outside_range) {
      keys_[hole] = keys_[j];
      slots_[hole] = slots_[j];
      hole = j;
    }
  }
  keys_[hole] = 0;
  slots_[hole] = kNoIndex;
  --size_;
  return true;
}

std::uint32_t OrderIndex::find(OrderId id) const noexcept {
  if (id == kInvalidId) return kNoIndex;
  std::size_t i = static_cast<std::size_t>(hash(id)) & kMask;
  while (keys_[i] != 0) {
    if (keys_[i] == id) return slots_[i];
    i = (i + 1) & kMask;
  }
  return kNoIndex;
}

}  // namespace ultralob
