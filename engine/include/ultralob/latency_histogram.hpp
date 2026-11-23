// UltraLOB — high dynamic range latency histogram.
//
// Fixed log-linear bucketing (HdrHistogram-style idea, hand rolled so the
// native benchmark, the WASM benchmark and the tests share one implementation
// with no allocator involvement). Values below 32 ns are exact; above that a
// bucket spans one octave split into 32 sub-steps, so a reported percentile is
// the lower edge of its bucket with at most 1/32 (~3.1%) relative error, and no
// per-sample allocation.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ultralob {

class LatencyHistogram {
 public:
  static constexpr std::size_t kSubBucketBits = 5;                  // 32 linear steps
  static constexpr std::size_t kSubBuckets = 1u << kSubBucketBits;
  static constexpr std::size_t kBuckets = 44;                       // up to ~4.4s
  static constexpr std::size_t kSlots = kBuckets * kSubBuckets;

  void reset() noexcept {
    counts_.fill(0);
    total_ = 0;
  }

  void record(std::uint64_t value_ns) noexcept {
    counts_[index_of(value_ns)]++;
    ++total_;
  }

  std::uint64_t count() const noexcept { return total_; }

  // Percentile in [0, 100] with 0.01 granularity, e.g. 9999 => p99.99.
  std::uint64_t percentile(double pct) const noexcept {
    if (total_ == 0) return 0;
    std::uint64_t target =
        static_cast<std::uint64_t>((pct / 100.0) * static_cast<double>(total_));
    if (target == 0) target = 1;
    std::uint64_t running = 0;
    for (std::size_t i = 0; i < kSlots; ++i) {
      running += counts_[i];
      if (running >= target) return value_of(i);
    }
    return value_of(kSlots - 1);
  }

  std::uint64_t max() const noexcept {
    for (std::size_t i = kSlots; i-- > 0;) {
      if (counts_[i] != 0) return value_of(i);
    }
    return 0;
  }

  std::uint64_t min() const noexcept {
    for (std::size_t i = 0; i < kSlots; ++i) {
      if (counts_[i] != 0) return value_of(i);
    }
    return 0;
  }

  std::uint64_t mean() const noexcept {
    if (total_ == 0) return 0;
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < kSlots; ++i) {
      if (counts_[i] != 0) sum += counts_[i] * value_of(i);
    }
    return sum / total_;
  }

  static std::size_t index_of(std::uint64_t value_ns) noexcept {
    if (value_ns < kSubBuckets) return static_cast<std::size_t>(value_ns);
    std::size_t bucket = 0;
    std::uint64_t shifted = value_ns;
    while (shifted >= kSubBuckets * 2 && bucket + 1 < kBuckets) {
      shifted >>= 1;
      ++bucket;
    }
    const std::size_t sub =
        static_cast<std::size_t>(shifted & (kSubBuckets - 1));
    const std::size_t index = (bucket + 1) * kSubBuckets + sub;
    return index < kSlots ? index : kSlots - 1;
  }

  // Representative value for a slot (lower edge of the bucket range).
  static std::uint64_t value_of(std::size_t index) noexcept {
    const std::size_t bucket = index / kSubBuckets;
    const std::size_t sub = index % kSubBuckets;
    if (bucket == 0) return static_cast<std::uint64_t>(sub);
    const std::uint64_t base = static_cast<std::uint64_t>(kSubBuckets)
                               << static_cast<std::uint64_t>(bucket - 1);
    return base + static_cast<std::uint64_t>(sub);
  }

 private:
  std::array<std::uint64_t, kSlots> counts_{};
  std::uint64_t total_ = 0;
};

}  // namespace ultralob
