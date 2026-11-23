#include "ultralob/chaos.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "ultralob/config.hpp"
#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/matching_engine.hpp"
#include "ultralob/snapshot.hpp"
#include "ultralob/spsc_queue.hpp"

namespace ultralob {
namespace {

using EventStream = std::vector<Event>;

EventStream make_stream(const FeedConfig& config, std::size_t count) {
  EventStream events;
  events.reserve(count);
  FeedGenerator generator(config);
  for (std::size_t i = 0; i < count; ++i) events.push_back(generator.next());
  return events;
}

EventProcessor::Config default_processor_config() {
  EventProcessor::Config config{};
  config.gap_policy = GapPolicy::Buffer;
  config.duplicate_policy = DuplicatePolicy::Reject;
  config.start_seq = 1;
  return config;
}

// Reference state for a prefix of the stream, applied cleanly.
std::uint64_t replay_baseline(const EventStream& events, std::size_t count,
                              EventProcessor::Config config) {
  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config);
  for (std::size_t i = 0; i < count; ++i) processor.process(events[i]);
  return engine->crc64();
}

void set_detail(ChaosReport& report, const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vsnprintf(report.detail, sizeof(report.detail), format, args);
  va_end(args);
}

// ---------------------------------------------------------------------------
// 1. Sequence gap: a sequence number never arrives.
// ---------------------------------------------------------------------------
ChaosReport scenario_sequence_gap() {
  ChaosReport report{};
  report.scenario = ChaosScenario::SequenceGap;

  const EventStream events = make_stream(workload_config(Workload::Balanced, 0xA11CE), 240);

  EventProcessor::Config halt_config = default_processor_config();
  halt_config.gap_policy = GapPolicy::RejectAndHalt;

  // Baseline: the clean prefix 1..99.
  report.crc_baseline = replay_baseline(events, 99, halt_config);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, halt_config);
  for (std::size_t i = 0; i < 99; ++i) processor.process(events[i]);

  const ProcessOutcome gap = processor.process(events[100]);  // seq 101, seq 100 missing
  const ProcessOutcome after = processor.process(events[101]);  // must be refused

  report.gaps = processor.stats().gaps;
  report.applied = processor.stats().applied;
  report.detected = (gap.status == ProcessStatus::Gap) ? 1 : 0;
  report.crc_observed = engine->crc64();
  report.state_matches = report.crc_observed == report.crc_baseline;

  const bool refused_later = after.status == ProcessStatus::Halted;
  report.passed = report.detected == 1 && report.gaps == 1 && processor.halted() &&
                  refused_later && report.state_matches;
  set_detail(report,
             "seq %llu missing (expected %llu): GAP_DETECTED, halted=%d, later events "
             "refused=%d, book %s",
             static_cast<unsigned long long>(events[100].seq),
             static_cast<unsigned long long>(gap.expected_seq),
             processor.halted() ? 1 : 0, refused_later ? 1 : 0,
             report.state_matches ? "unchanged" : "CORRUPTED");
  return report;
}

// ---------------------------------------------------------------------------
// 2. Duplicate delivery: an already-applied event id arrives again.
// ---------------------------------------------------------------------------
ChaosReport scenario_duplicate_event() {
  ChaosReport report{};
  report.scenario = ChaosScenario::DuplicateEvent;

  const EventStream events = make_stream(workload_config(Workload::Balanced, 0xB0B), 200);
  const EventProcessor::Config config = default_processor_config();
  report.crc_baseline = replay_baseline(events, events.size(), config);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config);
  for (const Event& event : events) processor.process(event);

  const ProcessOutcome repeat = processor.process(events[49]);  // replay event id 50

  report.duplicates = processor.stats().duplicates;
  report.applied = processor.stats().applied;
  report.detected = (repeat.status == ProcessStatus::Duplicate) ? 1 : 0;
  report.crc_observed = engine->crc64();
  report.state_matches = report.crc_observed == report.crc_baseline;

  report.passed = report.detected == 1 && report.duplicates == 1 &&
                  report.state_matches && processor.stats().applied == events.size();
  set_detail(report,
             "replayed event id %llu: %s, applied-once=%llu/%llu, checksum %s",
             static_cast<unsigned long long>(events[49].id),
             process_status_name(repeat.status),
             static_cast<unsigned long long>(processor.stats().applied),
             static_cast<unsigned long long>(events.size()),
             report.state_matches ? "identical" : "DIVERGED");
  return report;
}

// ---------------------------------------------------------------------------
// 3. Out-of-order delivery: a later sequence arrives first, then the missing one.
// ---------------------------------------------------------------------------
ChaosReport scenario_out_of_order() {
  ChaosReport report{};
  report.scenario = ChaosScenario::OutOfOrderEvent;

  const EventStream events = make_stream(workload_config(Workload::Balanced, 0xC0FFEE), 160);
  const EventProcessor::Config config = default_processor_config();
  report.crc_baseline = replay_baseline(events, events.size(), config);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config);

  for (std::size_t i = 0; i < 59; ++i) processor.process(events[i]);  // seq 1..59
  const ProcessOutcome early = processor.process(events[60]);         // seq 61 arrives early
  const ProcessOutcome late_arrival = processor.process(events[59]);  // seq 60 fills the gap
  for (std::size_t i = 61; i < events.size(); ++i) processor.process(events[i]);

  const auto& stats = processor.stats();
  report.gaps = stats.gaps;
  report.buffered = stats.buffered;
  report.drained = stats.drained;
  report.out_of_order = stats.out_of_order;
  report.applied = stats.applied;
  report.detected = (early.status == ProcessStatus::Buffered) ? 1 : 0;
  report.crc_observed = engine->crc64();
  report.state_matches = report.crc_observed == report.crc_baseline;

  report.passed = report.detected == 1 && report.gaps == 1 && report.drained == 1 &&
                  late_arrival.status == ProcessStatus::Ok && report.state_matches;
  set_detail(report,
             "seq %llu buffered, seq %llu filled the gap and drained %llu event(s); "
             "final book %s",
             static_cast<unsigned long long>(events[60].seq),
             static_cast<unsigned long long>(events[59].seq),
             static_cast<unsigned long long>(report.drained),
             report.state_matches ? "identical to in-order run" : "DIVERGED");
  return report;
}

// ---------------------------------------------------------------------------
// 4. Consumer slowdown: bounded queue pressure under two producer policies.
// ---------------------------------------------------------------------------
ChaosReport scenario_consumer_slowdown() {
  ChaosReport report{};
  report.scenario = ChaosScenario::ConsumerSlowdown;

  const EventStream events = make_stream(workload_config(Workload::Balanced, 0xD00D), 4000);
  const EventProcessor::Config config = default_processor_config();
  report.crc_baseline = replay_baseline(events, events.size(), config);

  // Retry policy: a full queue never drops and never stalls forever — the
  // producer retries while the (deliberately slow) consumer makes progress.
  // Heap-allocated: a 256-slot queue of events is ~14 KiB of stack, which is
  // most of the WASM stack budget before any frames are pushed.
  auto queue_ptr = std::make_unique<SpscQueue<Event, 256>>();
  SpscQueue<Event, 256>& queue = *queue_ptr;
  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config);
  std::uint64_t consumed = 0;
  std::uint64_t retries = 0;

  auto consume_one = [&]() {
    Event popped{};
    if (queue.try_pop(popped)) {
      processor.process(popped);
      ++consumed;
    }
  };

  for (std::size_t i = 0; i < events.size(); ++i) {
    while (!queue.try_push(events[i])) {
      ++retries;
      consume_one();  // pressure forces the slow consumer to drain
    }
    if (i % 16 == 0) consume_one();
  }
  Event popped{};
  while (queue.try_pop(popped)) {
    processor.process(popped);
    ++consumed;
  }

  // Drop policy on the same throttled shape: losses are counted, never silent.
  auto drop_queue_ptr = std::make_unique<SpscQueue<Event, 64>>();
  SpscQueue<Event, 64>& drop_queue = *drop_queue_ptr;
  BoundedProducer<Event, 64> dropper(drop_queue, ProducerPolicy::Drop);
  std::uint64_t dropped_consumed = 0;
  for (std::size_t i = 0; i < events.size(); ++i) {
    dropper.push(events[i]);
    if (i % 32 == 0) {
      Event scratch{};
      if (drop_queue.try_pop(scratch)) ++dropped_consumed;
    }
  }

  report.retries = retries;
  report.dropped = dropper.counters().dropped;
  report.detected = retries > 0 ? 1 : 0;
  report.applied = processor.stats().applied;
  report.crc_observed = engine->crc64();
  report.state_matches = report.crc_observed == report.crc_baseline;

  const bool no_silent_loss = consumed == events.size() && report.applied == events.size();
  const bool drop_is_observable =
      report.dropped > 0 && dropper.counters().pushed + report.dropped == events.size();

  report.passed = no_silent_loss && report.state_matches && drop_is_observable;
  set_detail(report,
             "retry policy: %llu retries, %llu/%llu delivered, 0 lost; drop policy: %llu "
             "counted drops; book %s",
             static_cast<unsigned long long>(report.retries),
             static_cast<unsigned long long>(consumed),
             static_cast<unsigned long long>(events.size()),
             static_cast<unsigned long long>(report.dropped),
             report.state_matches ? "identical" : "DIVERGED");
  return report;
}

// ---------------------------------------------------------------------------
// 5. Snapshot recovery: serialize a book image, restore it, continue both.
// ---------------------------------------------------------------------------
ChaosReport scenario_snapshot_recovery() {
  ChaosReport report{};
  report.scenario = ChaosScenario::SnapshotRecovery;

  const EventStream events = make_stream(workload_config(Workload::CancelHeavy, 0x5151), 4000);
  const EventProcessor::Config config = default_processor_config();
  report.crc_baseline = replay_baseline(events, events.size(), config);

  auto live = std::make_unique<MatchingEngine>();
  EventProcessor live_processor(*live, config);
  for (std::size_t i = 0; i < 2000; ++i) live_processor.process(events[i]);

  const std::size_t image_orders = live->active_orders();
  std::vector<std::uint8_t> image(book_image_size(*live));
  std::size_t written = 0;
  const bool encoded = encode_book_image(*live, live_processor.last_applied_seq(), image.data(),
                                         image.size(), &written);

  auto recovered = std::make_unique<MatchingEngine>();
  std::uint64_t recovered_seq = 0;
  const bool restored =
      encoded && restore_book_image(*recovered, image.data(), written, &recovered_seq);

  const bool crc_round_trip = restored && recovered->crc64() == live->crc64();

  // Resume both engines from the checkpoint and compare the end state.
  EventProcessor::Config resume_config = config;
  resume_config.start_seq = recovered_seq + 1;
  EventProcessor recovered_processor(*recovered, resume_config);
  for (std::size_t i = 2000; i < events.size(); ++i) {
    live_processor.process(events[i]);
    recovered_processor.process(events[i]);
  }

  report.applied = live_processor.stats().applied;
  report.detected = crc_round_trip ? 1 : 0;
  report.crc_observed = recovered->crc64();
  report.state_matches = recovered->crc64() == live->crc64() &&
                         recovered->crc64() == report.crc_baseline;
  report.passed = encoded && restored && crc_round_trip && report.state_matches;
  set_detail(report,
             "image of %zu orders at seq %llu (%zu bytes) restored: %s, resumed %llu events, "
             "state %s",
             image_orders, static_cast<unsigned long long>(recovered_seq), written,
             crc_round_trip ? "checksum verified" : "CHECKSUM MISMATCH",
             static_cast<unsigned long long>(events.size() - 2000),
             report.state_matches ? "identical to uninterrupted run" : "DIVERGED");
  return report;
}

// ---------------------------------------------------------------------------
// 6. Invalid input: rejects must never mutate the book.
// ---------------------------------------------------------------------------
ChaosReport scenario_invalid_order() {
  ChaosReport report{};
  report.scenario = ChaosScenario::InvalidOrder;

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, default_processor_config());
  report.crc_baseline = engine->crc64();  // empty book

  Seq seq = 1;
  auto make_event = [&](EventType type, Side side, OrderType order_type, Price price, Qty qty,
                        OrderId order_id) {
    Event event{};
    event.seq = seq++;
    event.id = event.seq;
    event.type = type;
    event.side = side;
    event.order_type = order_type;
    event.price = price;
    event.qty = qty;
    event.order_id = order_id;
    return event;
  };

  const Event cases[] = {
      make_event(EventType::NewOrder, Side::Buy, OrderType::Limit, 10000, 0, kInvalidId),
      make_event(EventType::NewOrder, Side::Sell, OrderType::Limit, 0, 10, kInvalidId),
      make_event(EventType::NewOrder, Side::Buy, OrderType::Limit, -5, 10, kInvalidId),
      make_event(EventType::NewOrder, Side::Buy, OrderType::Limit, 10000,
                 Limits::kMaxOrderQty + 1, kInvalidId),
      make_event(EventType::CancelOrder, Side::Buy, OrderType::Limit, 0, 0, 999),
      make_event(EventType::ModifyOrder, Side::Buy, OrderType::Limit, 0, 0, 999),
      make_event(EventType::NewOrder, Side::Buy, OrderType::Market, 0, 0, kInvalidId),
  };

  std::uint64_t rejected = 0;
  for (const Event& event : cases) {
    const ProcessOutcome outcome = processor.process(event);
    if (outcome.status == ProcessStatus::Rejected) ++rejected;
  }

  report.rejected = rejected;
  report.detected = rejected;
  report.applied = processor.stats().applied;
  report.crc_observed = engine->crc64();
  const bool book_untouched = report.crc_observed == report.crc_baseline;

  // The engine must still be usable after the rejects.
  Event valid{};
  valid.seq = seq++;
  valid.id = valid.seq;
  valid.type = EventType::NewOrder;
  valid.side = Side::Buy;
  valid.order_type = OrderType::Limit;
  valid.price = 9900;
  valid.qty = 25;
  const ProcessOutcome accepted = processor.process(valid);

  report.state_matches = book_untouched;
  report.passed = rejected == 7 && book_untouched &&
                  accepted.status == ProcessStatus::Ok && engine->active_orders() == 1;
  set_detail(report,
             "%llu/%zu invalid events rejected, book %s, next valid order accepted=%d",
             static_cast<unsigned long long>(rejected), sizeof(cases) / sizeof(cases[0]),
             book_untouched ? "unchanged" : "CORRUPTED",
             accepted.status == ProcessStatus::Ok ? 1 : 0);
  return report;
}

}  // namespace

const char* chaos_scenario_id(ChaosScenario scenario) noexcept {
  switch (scenario) {
    case ChaosScenario::SequenceGap: return "sequence_gap";
    case ChaosScenario::DuplicateEvent: return "duplicate_event";
    case ChaosScenario::OutOfOrderEvent: return "out_of_order_event";
    case ChaosScenario::ConsumerSlowdown: return "consumer_slowdown";
    case ChaosScenario::SnapshotRecovery: return "snapshot_recovery";
    case ChaosScenario::InvalidOrder: return "invalid_order";
  }
  return "unknown";
}

const char* chaos_scenario_title(ChaosScenario scenario) noexcept {
  switch (scenario) {
    case ChaosScenario::SequenceGap: return "Sequence gap";
    case ChaosScenario::DuplicateEvent: return "Duplicate event";
    case ChaosScenario::OutOfOrderEvent: return "Out-of-order event";
    case ChaosScenario::ConsumerSlowdown: return "Consumer slowdown";
    case ChaosScenario::SnapshotRecovery: return "Snapshot recovery";
    case ChaosScenario::InvalidOrder: return "Invalid order";
  }
  return "Unknown";
}

const char* chaos_scenario_description(ChaosScenario scenario) noexcept {
  switch (scenario) {
    case ChaosScenario::SequenceGap: return "Skip a sequence number in the stream.";
    case ChaosScenario::DuplicateEvent: return "Re-deliver an event id that was already applied.";
    case ChaosScenario::OutOfOrderEvent: return "Deliver a later sequence before the one it follows.";
    case ChaosScenario::ConsumerSlowdown: return "Throttle the SPSC consumer against a bounded queue.";
    case ChaosScenario::SnapshotRecovery: return "Restore a serialized book image and resume the stream.";
    case ChaosScenario::InvalidOrder: return "Submit malformed orders and cancel/modify unknown ids.";
  }
  return "";
}

ChaosReport run_chaos_scenario(ChaosScenario scenario) noexcept {
  switch (scenario) {
    case ChaosScenario::SequenceGap: return scenario_sequence_gap();
    case ChaosScenario::DuplicateEvent: return scenario_duplicate_event();
    case ChaosScenario::OutOfOrderEvent: return scenario_out_of_order();
    case ChaosScenario::ConsumerSlowdown: return scenario_consumer_slowdown();
    case ChaosScenario::SnapshotRecovery: return scenario_snapshot_recovery();
    case ChaosScenario::InvalidOrder: return scenario_invalid_order();
  }
  ChaosReport report{};
  report.scenario = scenario;
  set_detail(report, "unknown scenario");
  return report;
}

std::size_t run_all_chaos(ChaosReport* out, std::size_t capacity) noexcept {
  if (out == nullptr) return 0;
  std::size_t count = 0;
  for (std::size_t i = 0; i < kChaosScenarioCount && count < capacity; ++i) {
    out[count++] = run_chaos_scenario(static_cast<ChaosScenario>(i));
  }
  return count;
}

namespace {

// Escapes a detail string for JSON. Details are generated by this file, but a
// quote or backslash in a future detail must not be able to corrupt the payload
// the browser parses.
void escape_json(const char* input, char* out, std::size_t out_size) noexcept {
  if (out == nullptr || out_size == 0) return;
  std::size_t written = 0;
  for (const char* cursor = input; cursor != nullptr && *cursor != '\0'; ++cursor) {
    const char* piece = nullptr;
    char buffer[8];
    switch (*cursor) {
      case '"': piece = "\\\""; break;
      case '\\': piece = "\\\\"; break;
      case '\n': piece = "\\n"; break;
      case '\r': piece = "\\r"; break;
      case '\t': piece = "\\t"; break;
      default: break;
    }
    if (piece == nullptr) {
      if (static_cast<unsigned char>(*cursor) < 0x20) {
        std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                      static_cast<unsigned>(static_cast<unsigned char>(*cursor)));
        piece = buffer;
      } else {
        buffer[0] = *cursor;
        buffer[1] = '\0';
        piece = buffer;
      }
    }
    const std::size_t len = std::strlen(piece);
    if (written + len + 1 >= out_size) break;  // keep room for the terminator
    std::memcpy(out + written, piece, len);
    written += len;
  }
  out[written] = '\0';
}

}  // namespace

void chaos_report_to_json(const ChaosReport& report, char* out,
                          std::size_t out_size) noexcept {
  if (out == nullptr || out_size == 0) return;
  char detail[256];
  escape_json(report.detail, detail, sizeof(detail));
  out[0] = '\0';
  std::snprintf(
      out, out_size,
      "{\"scenario\":\"%s\",\"title\":\"%s\",\"description\":\"%s\",\"passed\":%s,"
      "\"detected\":%llu,\"applied\":%llu,\"rejected\":%llu,\"duplicates\":%llu,"
      "\"gaps\":%llu,\"out_of_order\":%llu,\"buffered\":%llu,\"drained\":%llu,"
      "\"dropped\":%llu,\"retries\":%llu,\"crc_baseline\":\"%llu\",\"crc_observed\":\"%llu\","
      "\"state_matches\":%s,\"detail\":\"%s\"}",
      chaos_scenario_id(report.scenario), chaos_scenario_title(report.scenario),
      chaos_scenario_description(report.scenario), report.passed ? "true" : "false",
      static_cast<unsigned long long>(report.detected),
      static_cast<unsigned long long>(report.applied),
      static_cast<unsigned long long>(report.rejected),
      static_cast<unsigned long long>(report.duplicates),
      static_cast<unsigned long long>(report.gaps),
      static_cast<unsigned long long>(report.out_of_order),
      static_cast<unsigned long long>(report.buffered),
      static_cast<unsigned long long>(report.drained),
      static_cast<unsigned long long>(report.dropped),
      static_cast<unsigned long long>(report.retries),
      static_cast<unsigned long long>(report.crc_baseline),
      static_cast<unsigned long long>(report.crc_observed),
      report.state_matches ? "true" : "false", detail);
  // snprintf truncates (and can be told to) but must never leave the buffer
  // unterminated: the browser parses this as JSON.
  out[out_size - 1] = '\0';
}

}  // namespace ultralob
