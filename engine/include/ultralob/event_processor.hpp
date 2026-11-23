// UltraLOB — sequence-aware event processor.
//
// Every mutation enters the engine as an ordered event. The processor is the
// only component allowed to call the matching engine, and it enforces:
//   * exactly-once application of a sequence number,
//   * duplicate event-id detection (direct-mapped window over recent ids),
//   * gap detection with a configurable policy (halt, reject, or buffer),
//   * bounded reordering so a late event can still be applied in order.
//
// Nothing here silently repairs a stream: every anomaly is counted and
// reported through ProcessOutcome.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ultralob/config.hpp"
#include "ultralob/event.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/types.hpp"

namespace ultralob {

enum class GapPolicy : std::uint8_t {
  RejectAndHalt = 0,    // stop processing; operator must recover from snapshot
  RejectAndContinue = 1,  // drop the anomaly, keep advancing on new events
  Buffer = 2,           // hold later events until the gap is filled (bounded)
};

enum class DuplicatePolicy : std::uint8_t {
  Reject = 0,  // report Duplicate, count an error, do not apply
  Skip = 1,    // report Ok-but-skipped; still never applies twice
};

enum class ProcessStatus : std::uint8_t {
  Ok = 0,
  Rejected = 1,     // engine-level rejection (bad order, unknown id, ...)
  Duplicate = 2,
  Gap = 3,
  OutOfOrder = 4,
  Buffered = 5,
  Halted = 6,
};

const char* process_status_name(ProcessStatus status) noexcept;

struct ProcessOutcome {
  ProcessStatus status = ProcessStatus::Ok;
  RejectReason reason = RejectReason::None;
  Seq seq = 0;
  Seq expected_seq = 0;
  std::uint32_t trades = 0;
  bool applied = false;
};

class EventProcessor {
 public:
  struct Config {
    GapPolicy gap_policy = GapPolicy::Buffer;
    DuplicatePolicy duplicate_policy = DuplicatePolicy::Reject;
    Seq start_seq = 1;
  };

  struct Stats {
    std::uint64_t received = 0;
    std::uint64_t applied = 0;
    std::uint64_t engine_rejected = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t gaps = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t buffered = 0;
    std::uint64_t drained = 0;
    std::uint64_t buffer_overflows = 0;
    std::uint64_t halted_events = 0;
  };

  explicit EventProcessor(MatchingEngine& engine) noexcept;
  EventProcessor(MatchingEngine& engine, Config config) noexcept;

  void reset(Config config) noexcept;

  ProcessOutcome process(const Event& event) noexcept;

  Seq last_applied_seq() const noexcept { return last_applied_; }
  Seq expected_seq() const noexcept { return last_applied_ + 1; }
  bool halted() const noexcept { return halted_; }
  std::size_t buffered() const noexcept { return buffer_count_; }
  const Config& config() const noexcept { return config_; }
  const Stats& stats() const noexcept { return stats_; }

 private:
  ProcessOutcome apply(const Event& event) noexcept;
  bool seen_event_id(std::uint64_t id) const noexcept;
  void remember_event_id(std::uint64_t id) noexcept;
  void drain_buffer() noexcept;

  MatchingEngine* engine_;
  Config config_;
  Seq last_applied_ = 0;
  bool halted_ = false;

  // Direct-mapped, single-probe duplicate window: catches replayed ids inside
  // the last Limits::kRecentEventIds distinct events without per-event scans.
  std::array<std::uint64_t, Limits::kRecentEventIds> recent_ids_{};

  std::array<Event, Limits::kReorderBuffer> buffer_{};
  std::size_t buffer_count_ = 0;

  Stats stats_{};
};

}  // namespace ultralob
