// Test-only instrumentation: counts every heap allocation made through the
// global operator new/delete in the test binary. This is how the "no heap
// allocation in steady state" claim is verified instead of assumed.
#pragma once

#include <cstdint>

namespace ultralob_test {

struct AllocStats {
  std::uint64_t allocations = 0;
  std::uint64_t deallocations = 0;
  std::uint64_t bytes = 0;
};

AllocStats allocation_stats() noexcept;
void reset_allocation_stats() noexcept;

}  // namespace ultralob_test
