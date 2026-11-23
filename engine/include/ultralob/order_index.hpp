// UltraLOB — order id index.
//
// Cancels and modifies must be O(1) on a book that can hold 131k resting
// orders, so order ids are mapped to pool slots with an open-addressing hash
// table over fixed arrays: no allocation, no tombstones (backward-shift
// deletion keeps probe chains valid under heavy churn).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ultralob/types.hpp"

namespace ultralob {

class OrderIndex {
 public:
  static constexpr std::size_t kCapacity = 1u << 18;  // 262,144 slots
  static constexpr std::size_t kMask = kCapacity - 1;

  void clear() noexcept;
  bool insert(OrderId id, std::uint32_t slot) noexcept;
  bool erase(OrderId id) noexcept;
  std::uint32_t find(OrderId id) const noexcept;
  std::size_t size() const noexcept { return size_; }

  static std::uint64_t hash(OrderId id) noexcept {
    // splitmix64 finalizer: id space is caller controlled, hash it properly.
    std::uint64_t x = id + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
  }

 private:
  std::array<OrderId, kCapacity> keys_{};          // 0 marks an empty slot
  std::array<std::uint32_t, kCapacity> slots_{};
  std::size_t size_ = 0;
};

}  // namespace ultralob
