#include "ultralob/event_processor.hpp"

#include "ultralob/order_index.hpp"

namespace ultralob {

const char* process_status_name(ProcessStatus status) noexcept {
  switch (status) {
    case ProcessStatus::Ok: return "ok";
    case ProcessStatus::Rejected: return "rejected";
    case ProcessStatus::Duplicate: return "duplicate";
    case ProcessStatus::Gap: return "gap";
    case ProcessStatus::OutOfOrder: return "out_of_order";
    case ProcessStatus::Buffered: return "buffered";
    case ProcessStatus::Halted: return "halted";
  }
  return "unknown";
}

EventProcessor::EventProcessor(MatchingEngine& engine) noexcept : engine_(&engine) {
  Config defaults{};
  reset(defaults);
}

EventProcessor::EventProcessor(MatchingEngine& engine, Config config) noexcept
    : engine_(&engine) {
  reset(config);
}

void EventProcessor::reset(Config config) noexcept {
  config_ = config;
  last_applied_ = config.start_seq == 0 ? 0 : config.start_seq - 1;
  halted_ = false;
  recent_ids_.fill(0);
  buffer_count_ = 0;
  stats_ = Stats{};
}

bool EventProcessor::seen_event_id(std::uint64_t id) const noexcept {
  if (id == 0) return false;
  const std::size_t slot =
      static_cast<std::size_t>(OrderIndex::hash(id)) & (Limits::kRecentEventIds - 1);
  return recent_ids_[slot] == id;
}

void EventProcessor::remember_event_id(std::uint64_t id) noexcept {
  if (id == 0) return;
  const std::size_t slot =
      static_cast<std::size_t>(OrderIndex::hash(id)) & (Limits::kRecentEventIds - 1);
  recent_ids_[slot] = id;
}

ProcessOutcome EventProcessor::process(const Event& event) noexcept {
  ++stats_.received;
  ProcessOutcome outcome{};
  outcome.seq = event.seq;
  outcome.expected_seq = expected_seq();

  if (halted_) {
    outcome.status = ProcessStatus::Halted;
    ++stats_.halted_events;
    return outcome;
  }

  if (event.seq == 0) {
    outcome.status = ProcessStatus::OutOfOrder;
    ++stats_.out_of_order;
    return outcome;
  }

  if (seen_event_id(event.id)) {
    ++stats_.duplicates;
    outcome.status = config_.duplicate_policy == DuplicatePolicy::Skip
                         ? ProcessStatus::Ok
                         : ProcessStatus::Duplicate;
    return outcome;
  }

  const Seq expected = expected_seq();

  if (event.seq < expected) {
    // Position already consumed by a different event id: a re-delivery.
    ++stats_.out_of_order;
    outcome.status = event.seq == last_applied_ ? ProcessStatus::Duplicate
                                                : ProcessStatus::OutOfOrder;
    return outcome;
  }

  if (event.seq > expected) {
    ++stats_.gaps;
    outcome.status = ProcessStatus::Gap;
    switch (config_.gap_policy) {
      case GapPolicy::RejectAndHalt:
        halted_ = true;
        return outcome;
      case GapPolicy::RejectAndContinue:
        // Resynchronise: the missing range is declared permanently lost and the
        // stream continues at the next sequence number. The gap event itself is
        // not applied because its position in the book is ambiguous.
        last_applied_ = event.seq;
        return outcome;
      case GapPolicy::Buffer:
        if (buffer_count_ >= buffer_.size()) {
          ++stats_.buffer_overflows;
          return outcome;
        }
        buffer_[buffer_count_++] = event;
        ++stats_.buffered;
        outcome.status = ProcessStatus::Buffered;
        return outcome;
    }
  }

  outcome = apply(event);
  drain_buffer();
  return outcome;
}

ProcessOutcome EventProcessor::apply(const Event& event) noexcept {
  ProcessOutcome outcome{};
  outcome.seq = event.seq;
  outcome.expected_seq = expected_seq();

  SubmitResult result{};
  switch (event.type) {
    case EventType::NewOrder:
      if (event.order_type == OrderType::Market) {
        result = engine_->submit_market(event.side, event.qty, event.seq, event.order_id);
      } else {
        result = engine_->submit_limit(event.side, event.price, event.qty, event.seq,
                                       event.order_id);
      }
      break;
    case EventType::CancelOrder:
      result = engine_->cancel(event.order_id, event.seq);
      break;
    case EventType::ModifyOrder:
      result = engine_->modify(event.order_id, event.price, event.qty, event.seq);
      break;
    case EventType::TradeExecuted:
    case EventType::SnapshotCreated:
    case EventType::GapDetected:
    case EventType::RecoveryCompleted:
      // Control-plane events are recorded, not applied to the book.
      result.accepted = true;
      break;
  }

  // The sequence is consumed even when the engine rejects the business action:
  // a rejected order is a valid stream position, not a stream anomaly.
  last_applied_ = event.seq;
  ++stats_.applied;
  remember_event_id(event.id);

  outcome.applied = true;
  outcome.trades = result.trade_count;
  if (result.accepted) {
    outcome.status = ProcessStatus::Ok;
  } else {
    outcome.status = ProcessStatus::Rejected;
    outcome.reason = result.reason;
    ++stats_.engine_rejected;
  }
  outcome.expected_seq = expected_seq();
  return outcome;
}

void EventProcessor::drain_buffer() noexcept {
  bool progressed = true;
  while (progressed) {
    progressed = false;
    for (std::size_t i = 0; i < buffer_count_; ++i) {
      if (buffer_[i].seq == expected_seq()) {
        const Event event = buffer_[i];
        buffer_[i] = buffer_[buffer_count_ - 1];
        --buffer_count_;
        apply(event);
        ++stats_.drained;
        progressed = true;
        break;
      }
    }
  }
}

}  // namespace ultralob
