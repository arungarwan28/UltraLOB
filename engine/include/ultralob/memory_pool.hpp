// UltraLOB — fixed capacity, index-based object pool.
//
// Allocation is a free-list pop (O(1), no heap traffic, no pointer chasing).
// The pool exposes instrumentation counters so tests and the benchmark can
// state what actually happened instead of guessing.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ultralob/types.hpp"

namespace ultralob {

template <typename T, std::size_t Capacity>
class ObjectPool {
 public:
  ObjectPool() { reset(); }

  void reset() noexcept {
    free_head_ = 0;
    for (std::uint32_t i = 0; i < Capacity; ++i) {
      next_free_[i] = (i + 1 < Capacity) ? (i + 1) : kNoIndex;
      live_[i] = 0;
    }
    live_count_ = 0;
    high_water_ = 0;
    acquire_calls_ = 0;
  }

  // Returns kNoIndex when the pool is exhausted. Never throws, never allocates.
  std::uint32_t acquire() noexcept {
    if (free_head_ == kNoIndex) {
      exhausted_events_++;
      return kNoIndex;
    }
    const std::uint32_t slot = free_head_;
    free_head_ = next_free_[slot];
    live_[slot] = 1;
    ++live_count_;
    ++acquire_calls_;
    if (live_count_ > high_water_) high_water_ = live_count_;
    return slot;
  }

  void release(std::uint32_t slot) noexcept {
    if (slot >= Capacity || live_[slot] == 0) return;
    live_[slot] = 0;
    --live_count_;
    next_free_[slot] = free_head_;
    free_head_ = slot;
  }

  T& operator[](std::uint32_t slot) noexcept { return slots_[slot]; }
  const T& operator[](std::uint32_t slot) const noexcept { return slots_[slot]; }

  bool is_live(std::uint32_t slot) const noexcept {
    return slot < Capacity && live_[slot] != 0;
  }

  std::size_t live() const noexcept { return live_count_; }
  std::size_t high_water_mark() const noexcept { return high_water_; }
  std::uint64_t acquire_calls() const noexcept { return acquire_calls_; }
  std::uint64_t exhausted_events() const noexcept { return exhausted_events_; }
  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  std::array<T, Capacity> slots_{};
  std::array<std::uint32_t, Capacity> next_free_{};
  std::array<std::uint8_t, Capacity> live_{};
  std::uint32_t free_head_ = kNoIndex;
  std::size_t live_count_ = 0;
  std::size_t high_water_ = 0;
  std::uint64_t acquire_calls_ = 0;
  std::uint64_t exhausted_events_ = 0;
};

}  // namespace ultralob
