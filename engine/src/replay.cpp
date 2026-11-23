#include "ultralob/replay.hpp"

namespace ultralob {

ReplaySession::ReplaySession(MatchingEngine& engine, ReplayConfig config) noexcept
    : engine_(engine), config_(config), feed_(config.feed),
      processor_(engine, config.processor) {}

ProcessOutcome ReplaySession::step() noexcept {
  const Event event = feed_.next();
  ++generated_;
  return processor_.process(event);
}

std::uint64_t ReplaySession::run(std::uint64_t max_events) noexcept {
  std::uint64_t processed = 0;
  while (max_events == 0 || processed < max_events) {
    const ProcessOutcome outcome = step();
    ++processed;
    if (outcome.status == ProcessStatus::Halted) break;
  }
  return processed;
}

Snapshot ReplaySession::snapshot() const noexcept {
  return take_snapshot(engine_, generated_, processor_.last_applied_seq());
}

}  // namespace ultralob
