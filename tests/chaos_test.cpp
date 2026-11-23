#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

#include "ultralob/chaos.hpp"

namespace {
using namespace ultralob;
}  // namespace

TEST(Chaos, EveryScenarioPassesItsOwnExpectation) {
  for (std::size_t i = 0; i < kChaosScenarioCount; ++i) {
    const ChaosScenario scenario = static_cast<ChaosScenario>(i);
    const ChaosReport report = run_chaos_scenario(scenario);
    EXPECT_TRUE(report.passed) << chaos_scenario_id(scenario) << ": " << report.detail;
    EXPECT_EQ(report.scenario, scenario);
    EXPECT_GT(std::strlen(report.detail), 0u);
  }
}

TEST(Chaos, SequenceGapHaltsWithoutCorruptingState) {
  const ChaosReport report = run_chaos_scenario(ChaosScenario::SequenceGap);
  EXPECT_TRUE(report.passed);
  EXPECT_EQ(report.gaps, 1u);
  EXPECT_TRUE(report.state_matches);
}

TEST(Chaos, DuplicateEventIsDetectedOnce) {
  const ChaosReport report = run_chaos_scenario(ChaosScenario::DuplicateEvent);
  EXPECT_TRUE(report.passed);
  EXPECT_EQ(report.duplicates, 1u);
  EXPECT_EQ(report.crc_observed, report.crc_baseline);
}

TEST(Chaos, ConsumerSlowdownLosesNothingUnderRetry) {
  const ChaosReport report = run_chaos_scenario(ChaosScenario::ConsumerSlowdown);
  EXPECT_TRUE(report.passed);
  EXPECT_EQ(report.dropped > 0u, true);  // drop policy is observable, not silent
  EXPECT_GT(report.retries, 0u);
  EXPECT_TRUE(report.state_matches);
}

TEST(Chaos, SnapshotRecoveryResumesIdentically) {
  const ChaosReport report = run_chaos_scenario(ChaosScenario::SnapshotRecovery);
  EXPECT_TRUE(report.passed);
  EXPECT_EQ(report.crc_observed, report.crc_baseline);
}

TEST(Chaos, ReportsSerialiseToJson) {
  const ChaosReport report = run_chaos_scenario(ChaosScenario::InvalidOrder);
  char json[1200];
  chaos_report_to_json(report, json, sizeof(json));
  EXPECT_NE(std::strstr(json, "\"scenario\":\"invalid_order\""), nullptr);
  EXPECT_NE(std::strstr(json, "\"state_matches\":true"), nullptr);
}
