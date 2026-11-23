// Benchmark-side allocation instrumentation.
//
// Overrides the global operator new/delete so the benchmark can state how many
// heap allocations happened *inside the measured region*. Scope matters: this
// measures the process's C++ allocations, not every possible allocation path
// (allocator internals, stdio buffers, thread stacks). docs/benchmarking.md
// states exactly that.
#pragma once

#include <cstdint>

namespace ultralob_bench {

struct AllocationSnapshot {
  std::uint64_t allocations = 0;
  std::uint64_t deallocations = 0;
  std::uint64_t bytes = 0;
};

AllocationSnapshot allocations() noexcept;
void reset_allocations() noexcept;

}  // namespace ultralob_bench
