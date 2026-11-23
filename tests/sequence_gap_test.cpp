#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "ultralob/event_processor.hpp"
#include "ultralob/feed_generator.hpp"
#include "ultralob/matching_engine.hpp"

namespace {
using namespace ultralob;

std::vector<Event> make_stream(std::uint64_t seed, std::size_t count) {
  std::vector<Event> events;
  events.reserve(count);
  FeedGenerator generator(workload_config(Workload::Balanced, seed));
  for (std::size_t i = 0; i < count; ++i) events.push_back(generator.next());
  return events;
}

EventProcessor::Config config_with(GapPolicy policy) {
  EventProcessor::Config config{};
  config.gap_policy = policy;
  config.duplicate_policy = DuplicatePolicy::Reject;
  config.start_seq = 1;
  return config;
}

std::uint64_t baseline_crc(const std::vector<Event>& events, std::size_t count) {
  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config_with(GapPolicy::RejectAndContinue));
  for (std::size_t i = 0; i < count; ++i) processor.process(events[i]);
  return engine->crc64();
}
}  // namespace

// 50 controlled gap injections: the engine must detect every one of them.
TEST(SequenceGap, DetectsFiftyInjectedGaps) {
  const std::vector<Event> events = make_stream(0x6A6A, 400);
  int detected = 0;

  for (int injection = 0; injection < 50; ++injection) {
    const std::size_t gap_at = 20 + static_cast<std::size_t>(injection) * 7;

    auto engine = std::make_unique<MatchingEngine>();
    EventProcessor processor(*engine, config_with(GapPolicy::RejectAndContinue));

    for (std::size_t i = 0; i < gap_at; ++i) processor.process(events[i]);
    const Seq expected_before = processor.expected_seq();

    // Skip events[gap_at] entirely: deliver the event after it.
    const ProcessOutcome outcome = processor.process(events[gap_at + 1]);

    EXPECT_EQ(outcome.status, ProcessStatus::Gap) << "injection " << injection;
    EXPECT_EQ(outcome.expected_seq, expected_before) << "injection " << injection;
    EXPECT_EQ(processor.stats().gaps, 1u) << "injection " << injection;
    if (outcome.status == ProcessStatus::Gap) ++detected;

    // Resynchronised: the following event is applied (the engine may still
    // reject its business action), and no gap cascade occurs.
    const ProcessOutcome next = processor.process(events[gap_at + 2]);
    EXPECT_TRUE(next.applied) << "injection " << injection;
    EXPECT_NE(next.status, ProcessStatus::Gap) << "injection " << injection;
    EXPECT_EQ(processor.stats().gaps, 1u) << "injection " << injection;
  }

  EXPECT_EQ(detected, 50);
}

TEST(SequenceGap, HaltingPolicyLeavesBookUntouched) {
  const std::vector<Event> events = make_stream(0x7171, 200);
  const std::uint64_t expected = baseline_crc(events, 99);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config_with(GapPolicy::RejectAndHalt));
  for (std::size_t i = 0; i < 99; ++i) processor.process(events[i]);

  const ProcessOutcome gap = processor.process(events[100]);
  EXPECT_EQ(gap.status, ProcessStatus::Gap);
  EXPECT_TRUE(processor.halted());
  EXPECT_EQ(engine->crc64(), expected);

  const ProcessOutcome blocked = processor.process(events[101]);
  EXPECT_EQ(blocked.status, ProcessStatus::Halted);
  EXPECT_EQ(engine->crc64(), expected);  // still untouched
}

TEST(SequenceGap, BufferPolicyRecoversAndMatchesTheCleanRun) {
  const std::vector<Event> events = make_stream(0x8181, 120);
  const std::uint64_t expected = baseline_crc(events, events.size());

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config_with(GapPolicy::Buffer));

  for (std::size_t i = 0; i < 59; ++i) processor.process(events[i]);
  const ProcessOutcome early = processor.process(events[60]);  // seq 61 first
  EXPECT_EQ(early.status, ProcessStatus::Buffered);
  EXPECT_EQ(processor.buffered(), 1u);

  const ProcessOutcome fill = processor.process(events[59]);  // seq 60 closes the gap
  EXPECT_EQ(fill.status, ProcessStatus::Ok);
  EXPECT_EQ(processor.buffered(), 0u);
  EXPECT_EQ(processor.stats().drained, 1u);

  for (std::size_t i = 61; i < events.size(); ++i) processor.process(events[i]);

  EXPECT_EQ(processor.stats().gaps, 1u);
  EXPECT_EQ(processor.stats().applied, events.size());
  EXPECT_EQ(engine->crc64(), expected);
}

TEST(SequenceGap, DuplicateSequenceWithNewEventIdIsOutOfOrder) {
  const std::vector<Event> events = make_stream(0x9191, 20);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config_with(GapPolicy::Buffer));

  for (std::size_t i = 0; i < 10; ++i) processor.process(events[i]);

  Event replayed_position = events[4];
  replayed_position.id = 999999;  // different event id, already-consumed position
  const ProcessOutcome outcome = processor.process(replayed_position);
  EXPECT_EQ(outcome.status, ProcessStatus::OutOfOrder);
  EXPECT_EQ(processor.stats().out_of_order, 1u);
  EXPECT_EQ(processor.last_applied_seq(), 10u);
}

TEST(SequenceGap, DuplicateEventIdIsRejectedAndNotApplied) {
  const std::vector<Event> events = make_stream(0xA2A2, 40);

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config_with(GapPolicy::Buffer));
  for (const Event& event : events) processor.process(event);

  const std::uint64_t before = engine->crc64();
  const ProcessOutcome duplicate = processor.process(events[10]);
  EXPECT_EQ(duplicate.status, ProcessStatus::Duplicate);
  EXPECT_FALSE(duplicate.applied);
  EXPECT_EQ(engine->crc64(), before);
  EXPECT_EQ(processor.stats().duplicates, 1u);
}

TEST(SequenceGap, SkipPolicyCountsButSuppressesAnomaly) {
  const std::vector<Event> events = make_stream(0xB3B3, 30);

  EventProcessor::Config config = config_with(GapPolicy::Buffer);
  config.duplicate_policy = DuplicatePolicy::Skip;

  auto engine = std::make_unique<MatchingEngine>();
  EventProcessor processor(*engine, config);
  for (const Event& event : events) processor.process(event);

  const std::uint64_t before = engine->crc64();
  const ProcessOutcome duplicate = processor.process(events[5]);
  EXPECT_EQ(duplicate.status, ProcessStatus::Ok);
  EXPECT_FALSE(duplicate.applied);
  EXPECT_EQ(engine->crc64(), before);
  EXPECT_EQ(processor.stats().duplicates, 1u);
  EXPECT_EQ(processor.stats().applied, events.size());
}
