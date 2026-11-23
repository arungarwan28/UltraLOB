// The C ABI the browser calls, exercised natively.
//
// The browser build cannot be unit-tested from here, but the exports are plain
// C functions over the same engine, so a native test catches ABI-level defects
// — including the buffer bounds that the JS side cannot see.
//
// Regression: ul_trades() copied recent trades into a fixed 600-entry stack
// array while accepting up to kOutSlots/6 (1,365) trades, overflowing the stack
// for any caller that asked for more than 600.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ultralob/config.hpp"

extern "C" {
void ul_init(unsigned seed);
unsigned long long* ul_out_u64();
double* ul_out_f64();
int ul_submit_limit(int side, int price_ticks, int qty);
int ul_submit_market(int side, int qty);
int ul_cancel(unsigned order_id);
int ul_modify(unsigned order_id, int price_ticks, int qty);
int ul_last_status();
int ul_last_reject();
unsigned long long ul_last_order_id();
int ul_book_side(int side, int max_levels);
int ul_trades(int max);
double ul_best_bid();
double ul_best_ask();
double ul_spread();
int ul_has_order(unsigned order_id);
double ul_order_remaining(unsigned order_id);
int ul_side_depth(int side);
unsigned long long ul_seq();
unsigned long long ul_crc64();
unsigned long long ul_active_orders();
unsigned long long ul_trade_count();
unsigned long long ul_processed();
unsigned long long ul_rejected();
int ul_benchmark(unsigned events, unsigned seed, int workload_id);
void ul_replay_begin(unsigned events, unsigned seed, int workload_id, int gap_policy);
int ul_replay_step(unsigned steps);
unsigned long long ul_replay_generated();
unsigned long long ul_replay_crc64();
unsigned long long ul_replay_trades();
int ul_replay_verify();
const char* ul_chaos_json(int scenario);
const char* ul_chaos_all_json();
}

TEST(WasBindings, SubmitBookAndCountersRoundTrip) {
  ul_init(1);
  ASSERT_EQ(ul_active_orders(), 0u);

  const int bid = ul_submit_limit(0, 10000, 25);
  EXPECT_GT(bid, 0);
  const int ask = ul_submit_limit(1, 10010, 40);
  EXPECT_GT(ask, 0);
  EXPECT_GT(ask, bid);

  EXPECT_DOUBLE_EQ(ul_best_bid(), 10000.0);
  EXPECT_DOUBLE_EQ(ul_best_ask(), 10010.0);
  EXPECT_DOUBLE_EQ(ul_spread(), 10.0);
  EXPECT_EQ(ul_active_orders(), 2u);
  EXPECT_EQ(ul_side_depth(0), 1);
  EXPECT_EQ(ul_side_depth(1), 1);
  EXPECT_EQ(ul_has_order(static_cast<unsigned>(bid)), 1);
  EXPECT_DOUBLE_EQ(ul_order_remaining(static_cast<unsigned>(bid)), 25.0);

  // Levels land in the out buffers.
  ASSERT_GE(ul_book_side(0, 4), 1);
  unsigned long long* u64 = ul_out_u64();
  double* f64 = ul_out_f64();
  EXPECT_DOUBLE_EQ(f64[0], 10000.0);
  EXPECT_EQ(u64[0], 25u);
  EXPECT_EQ(u64[1], 1u);

  EXPECT_EQ(ul_cancel(static_cast<unsigned>(bid)), 1);
  EXPECT_EQ(ul_active_orders(), 1u);
  EXPECT_EQ(ul_cancel(static_cast<unsigned>(bid)), 0);  // already gone
  EXPECT_EQ(ul_last_status(), 1);                       // rejected
}

TEST(WasBindings, RejectionsAreReportedNotThrown) {
  ul_init(1);
  EXPECT_EQ(ul_submit_limit(0, -5, 10), 0);
  EXPECT_EQ(ul_last_status(), 1);
  EXPECT_EQ(ul_submit_limit(0, 10000, 0), 0);
  EXPECT_EQ(ul_submit_limit(0, 10000, static_cast<int>(ultralob::Limits::kMaxOrderQty + 1)), 0);
  EXPECT_EQ(ul_cancel(424242), 0);
  EXPECT_EQ(ul_active_orders(), 0u);
  EXPECT_GE(ul_rejected(), 4u);
}

TEST(WasBindings, TradeWindowClampsAndDoesNotOverflow) {
  ul_init(1);
  // 700 asks at increasing prices, then one market buy consumes all of them:
  // 700 trades, well past the old 600-entry stack window.
  for (int i = 0; i < 700; ++i) {
    ASSERT_GT(ul_submit_limit(1, 10000 + i, 1), 0) << i;
  }
  ASSERT_GT(ul_submit_market(0, 700), 0);
  EXPECT_EQ(ul_trade_count(), 700u);
  EXPECT_EQ(ul_active_orders(), 0u);

  // Ask for far more than the fitted window: must clamp, not corrupt memory.
  const int returned = ul_trades(5000);
  EXPECT_EQ(returned, 700);
  EXPECT_EQ(ul_trades(40), 40);

  unsigned long long* u64 = ul_out_u64();
  double* f64 = ul_out_f64();
  EXPECT_GT(f64[0], 0.0);
  EXPECT_GT(u64[0], 0u);  // trade id
}

TEST(WasBindings, BenchmarkRunsTheEngineAndReportsLatency) {
  ul_init(1);
  ASSERT_EQ(ul_benchmark(20'000, 2024, 0), 0);
  unsigned long long* u64 = ul_out_u64();
  double* f64 = ul_out_f64();
  EXPECT_EQ(u64[0], 20'000u);   // events
  EXPECT_GT(u64[1], 0u);        // trades
  EXPECT_GT(u64[4], 0u);        // crc64
  EXPECT_GT(u64[5], 0u);        // elapsed ns
  EXPECT_GE(u64[7], u64[6]);    // p99 >= p50
  EXPECT_GE(u64[10], u64[7]);   // max >= p99
  EXPECT_GT(f64[0], 0.0);       // events/second
}

TEST(WasBindings, ReplayCompareIsAnIndependentSecondRun) {
  ul_init(1);
  ul_replay_begin(4'000, 7, 0, 2);
  const int last = ul_replay_step(1'500);
  EXPECT_GE(last, 0);
  EXPECT_EQ(ul_replay_generated(), 1'500u);
  EXPECT_GT(ul_replay_crc64(), 0u);
  EXPECT_EQ(ul_replay_verify(), 1);  // live and reference checksums agree
  const unsigned long long live = ul_out_u64()[0];
  EXPECT_EQ(live, ul_out_u64()[1]);
}

TEST(WasBindings, ChaosJsonIsParseableShape) {
  ul_init(1);
  const std::string single(ul_chaos_json(0));
  EXPECT_EQ(single.rfind("{\"scenario\":\"sequence_gap\"", 0), 0u);
  EXPECT_NE(single.find("\"passed\":true"), std::string::npos);
  // CRC64 values exceed JavaScript's safe integer range, so they are emitted as
  // JSON strings (the console types them as strings and formats them).
  EXPECT_NE(single.find("\"crc_baseline\":\""), std::string::npos);
  EXPECT_NE(single.find("\"crc_observed\":\""), std::string::npos);

  const std::string all(ul_chaos_all_json());
  ASSERT_FALSE(all.empty());
  EXPECT_EQ(all.front(), '[');
  EXPECT_EQ(all.back(), ']');
  EXPECT_NE(all.find("snapshot_recovery"), std::string::npos);

  // Every scenario must be present and the payload must be large enough to be
  // the real array (regression: the WASM build returned an empty string here,
  // which surfaced in the console as "Unexpected end of JSON input").
  EXPECT_GT(all.size(), 1500u);
  int scenarios = 0;
  for (std::size_t at = all.find("\"scenario\""); at != std::string::npos;
       at = all.find("\"scenario\"", at + 1)) {
    ++scenarios;
  }
  EXPECT_EQ(scenarios, 6);
  // Balanced brackets: a truncated array is not valid JSON.
  int depth = 0;
  bool balanced = true;
  for (const char c : all) {
    if (c == '[') ++depth;
    if (c == ']') --depth;
    if (depth < 0) balanced = false;
  }
  EXPECT_TRUE(balanced);
  EXPECT_EQ(depth, 0);
}
