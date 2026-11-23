#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "ultralob/replay.hpp"
#include "ultralob/snapshot.hpp"

namespace {
using namespace ultralob;

ReplayConfig make_replay(std::uint64_t seed, Workload workload) {
  ReplayConfig config{};
  config.feed = workload_config(workload, seed);
  config.processor.gap_policy = GapPolicy::Buffer;
  config.processor.duplicate_policy = DuplicatePolicy::Reject;
  config.processor.start_seq = 1;
  return config;
}
}  // namespace

TEST(Determinism, SameStreamSameChecksumAndSnapshots) {
  constexpr std::uint64_t kEvents = 20000;

  auto first_engine = std::make_unique<MatchingEngine>();
  auto second_engine = std::make_unique<MatchingEngine>();

  ReplaySession first(*first_engine, make_replay(0xDEADBEEF, Workload::Balanced));
  ReplaySession second(*second_engine, make_replay(0xDEADBEEF, Workload::Balanced));

  first.run(kEvents);
  second.run(kEvents);

  const Snapshot a = first.snapshot();
  const Snapshot b = second.snapshot();

  EXPECT_EQ(a.crc64, b.crc64);
  EXPECT_TRUE(a.equals(b));
  EXPECT_EQ(a.seq, kEvents);
  EXPECT_GT(a.trades, 0u);
  EXPECT_GT(a.bid_levels + a.ask_levels, 0u);

  char diff[512];
  a.diff(b, diff, sizeof(diff));
  EXPECT_STREQ(diff, "");
}

TEST(Determinism, RepeatedRunsOfTheSameSessionObjectAgree) {
  auto engine = std::make_unique<MatchingEngine>();
  ReplayConfig config = make_replay(0x5A5A5A, Workload::Burst);

  ReplaySession session(*engine, config);
  session.run(5000);
  const Snapshot checkpoint = session.snapshot();

  // Continue to 15000 and remember the final state.
  session.run(10000);
  const Snapshot final_state = session.snapshot();

  // A fresh engine replaying the same stream in one go must match both
  // checkpoints exactly (checkpoint comparison, not just final state).
  auto fresh_engine = std::make_unique<MatchingEngine>();
  ReplaySession fresh(*fresh_engine, config);
  fresh.run(5000);
  const Snapshot fresh_checkpoint = fresh.snapshot();
  EXPECT_TRUE(checkpoint.equals(fresh_checkpoint));

  fresh.run(10000);
  const Snapshot fresh_final = fresh.snapshot();
  EXPECT_EQ(final_state.crc64, fresh_final.crc64);
  EXPECT_TRUE(final_state.equals(fresh_final));
}

TEST(Determinism, DifferentSeedsDiverge) {
  auto a = std::make_unique<MatchingEngine>();
  auto b = std::make_unique<MatchingEngine>();
  ReplaySession left(*a, make_replay(1, Workload::Balanced));
  ReplaySession right(*b, make_replay(2, Workload::Balanced));
  left.run(3000);
  right.run(3000);
  EXPECT_NE(left.snapshot().crc64, right.snapshot().crc64);
}

TEST(Determinism, ChecksumIsSensitiveToSingleOrderChange) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Buy, 10000, 10, 1);
  engine->submit_limit(Side::Buy, 9950, 10, 2);
  const std::uint64_t before = engine->crc64();

  engine->submit_limit(Side::Sell, 12000, 1, 3);  // far away, no trade
  const std::uint64_t after = engine->crc64();
  EXPECT_NE(before, after);
}

TEST(Determinism, ChecksumIsIndependentOfEventSequenceNumbers) {
  // Book state is what the checksum covers: the same orders submitted with
  // different (simulated) sequence numbers must hash identically.
  auto a = std::make_unique<MatchingEngine>();
  a->submit_limit(Side::Buy, 10000, 10, 1);
  a->submit_limit(Side::Sell, 10100, 10, 2);

  auto b = std::make_unique<MatchingEngine>();
  b->submit_limit(Side::Buy, 10000, 10, 900);
  b->submit_limit(Side::Sell, 10100, 10, 901);

  EXPECT_EQ(a->crc64(), b->crc64());
  EXPECT_EQ(a->best_bid(), b->best_bid());
}

TEST(Determinism, BookImageRoundTripRestoresTheSameChecksum) {
  auto live = std::make_unique<MatchingEngine>();
  ReplaySession session(*live, make_replay(0xFEEDFACE, Workload::CancelHeavy));
  session.run(4000);

  std::vector<std::uint8_t> image(book_image_size(*live));
  std::size_t written = 0;
  ASSERT_TRUE(encode_book_image(*live, session.processor().last_applied_seq(),
                                image.data(), image.size(), &written));

  auto recovered = std::make_unique<MatchingEngine>();
  std::uint64_t restored_seq = 0;
  ASSERT_TRUE(restore_book_image(*recovered, image.data(), written, &restored_seq));

  EXPECT_EQ(restored_seq, 4000u);
  EXPECT_EQ(recovered->crc64(), live->crc64());
  EXPECT_EQ(recovered->active_orders(), live->active_orders());
  EXPECT_EQ(recovered->best_bid(), live->best_bid());
  EXPECT_EQ(recovered->best_ask(), live->best_ask());

  // Top-of-book equality is necessary but not sufficient; compare full views.
  const Snapshot original = take_snapshot(*live, 4000, 4000);
  const Snapshot restored = take_snapshot(*recovered, 4000, 4000);
  EXPECT_TRUE(original.equals(restored));
}

TEST(Determinism, TruncatedImageIsRejected) {
  auto engine = std::make_unique<MatchingEngine>();
  engine->submit_limit(Side::Buy, 10000, 10, 1);
  engine->submit_limit(Side::Sell, 10100, 10, 2);

  std::vector<std::uint8_t> image(book_image_size(*engine));
  std::size_t written = 0;
  ASSERT_TRUE(encode_book_image(*engine, 2, image.data(), image.size(), &written));

  auto target = std::make_unique<MatchingEngine>();
  std::uint64_t seq = 0;
  EXPECT_FALSE(restore_book_image(*target, image.data(), written / 2, &seq));
  EXPECT_EQ(target->active_orders(), 0u);
}
