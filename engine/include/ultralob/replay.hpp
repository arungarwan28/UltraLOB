// UltraLOB — deterministic replay session.
//
// A replay is: feed generator -> event processor -> matching engine. Replaying
// the same (seed, workload, event count) twice must produce the same snapshot
// and the same CRC64; that is asserted by determinism_test.cpp, exposed in the
// replay lab and checked against the Python reference implementation.
#pragma once

#include <cstdint>

#include "ultralob/event.hpp"
#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/snapshot.hpp"
#include "ultralob/types.hpp"

namespace ultralob {

struct ReplayConfig {
  FeedConfig feed{};
  EventProcessor::Config processor{};
};

class ReplaySession {
 public:
  ReplaySession(MatchingEngine& engine, ReplayConfig config) noexcept;

  // Generates and applies exactly one event.
  ProcessOutcome step() noexcept;

  // Applies up to `max_events` events (0 = no limit) and returns how many were
  // generated. Stops early when the processor halts on a gap.
  std::uint64_t run(std::uint64_t max_events = 0) noexcept;

  Snapshot snapshot() const noexcept;
  Event peek_next() const noexcept { return feed_.peek(); }
  Seq generated() const noexcept { return generated_; }
  const ReplayConfig& config() const noexcept { return config_; }
  const EventProcessor& processor() const noexcept { return processor_; }
  const FeedGenerator& feed() const noexcept { return feed_; }

 private:
  MatchingEngine& engine_;
  ReplayConfig config_;
  FeedGenerator feed_;
  EventProcessor processor_;
  Seq generated_ = 0;
};

}  // namespace ultralob
