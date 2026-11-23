// UltraLOB — reliability / chaos scenarios.
//
// One implementation, three consumers: tests/chaos_test.cpp asserts the
// outcomes, the desktop CLI prints them as evidence, and the browser reliability
// lab renders exactly the same reports (so the UI cannot drift from the truth).
//
// Deliberately thread-free so the same code runs natively and in single-threaded
// WebAssembly; the threaded producer/consumer variant lives in
// tests/backpressure_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ultralob {

enum class ChaosScenario : std::uint32_t {
  SequenceGap = 0,
  DuplicateEvent = 1,
  OutOfOrderEvent = 2,
  ConsumerSlowdown = 3,
  SnapshotRecovery = 4,
  InvalidOrder = 5,
};

inline constexpr std::size_t kChaosScenarioCount = 6;

const char* chaos_scenario_id(ChaosScenario scenario) noexcept;
const char* chaos_scenario_title(ChaosScenario scenario) noexcept;
const char* chaos_scenario_description(ChaosScenario scenario) noexcept;

struct ChaosReport {
  ChaosScenario scenario = ChaosScenario::SequenceGap;
  bool passed = false;

  // Detection counters.
  std::uint64_t detected = 0;
  std::uint64_t applied = 0;
  std::uint64_t rejected = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t gaps = 0;
  std::uint64_t out_of_order = 0;
  std::uint64_t buffered = 0;
  std::uint64_t drained = 0;
  std::uint64_t dropped = 0;
  std::uint64_t retries = 0;

  // State verification.
  std::uint64_t crc_baseline = 0;
  std::uint64_t crc_observed = 0;
  bool state_matches = false;

  char detail[192]{};
};

ChaosReport run_chaos_scenario(ChaosScenario scenario) noexcept;

// Runs every scenario in declaration order. Returns the number written.
std::size_t run_all_chaos(ChaosReport* out, std::size_t capacity) noexcept;

// JSON rendering of one report (static buffer friendly, no allocation).
void chaos_report_to_json(const ChaosReport& report, char* out,
                          std::size_t out_size) noexcept;

}  // namespace ultralob
